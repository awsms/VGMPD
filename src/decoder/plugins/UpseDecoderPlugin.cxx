// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "UpseDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "Log.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "fs/Path.hxx"
#include "util/Domain.hxx"
#include "util/ScopeExit.hxx"
#include "util/StringCompare.hxx"

extern "C" {
#include <upse.h>
}

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string_view>

using std::string_view_literals::operator""sv;

static constexpr Domain upse_domain("upse");

static constexpr unsigned UPSE_CHANNELS = 2;
static constexpr unsigned UPSE_SAMPLE_RATE = 44100;

static bool upse_initialized = false;

static void *
Upse_fopen(const char *path, const char *mode)
{
	return std::fopen(path, mode);
}

static size_t
Upse_fread(void *buffer, size_t size, size_t count, void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::fread(buffer, size, count, f);
}

static int
Upse_fseek(void *handle, long offset, int whence)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::fseek(f, offset, whence);
}

static int
Upse_fclose(void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::fclose(f);
}

static long
Upse_ftell(void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::ftell(f);
}

static const upse_iofuncs_t upse_io = {
	Upse_fopen,
	Upse_fread,
	Upse_fseek,
	Upse_fclose,
	Upse_ftell,
};

extern "C" void upse_ps1_spu_setvolume(void *spu, int volume);

[[gnu::pure]]
static uint32_t
ParsePsfTimeMS(const char *ts) noexcept
{
	if (ts == nullptr || *ts == 0)
		return 0;

	if (std::strchr(ts, ':') == nullptr &&
	    std::strchr(ts, '.') == nullptr &&
	    std::strchr(ts, ',') == nullptr) {
		char *end = nullptr;
		unsigned long value = std::strtoul(ts, &end, 10);
		if (end == ts || *end != 0)
			return 0;

		/* Heuristic: large integer values are usually milliseconds. */
		const uint64_t ms = value >= 10000UL
			? static_cast<uint64_t>(value)
			: static_cast<uint64_t>(value) * 1000;
		return ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ms);
	}

	uint64_t total_seconds = 0;
	const char *p = ts;

	while (true) {
		const char *colon = std::strchr(p, ':');
		const char *end = colon != nullptr ? colon : p + std::strlen(p);
		bool seen_digit = false;
		bool seen_fraction = false;
		uint64_t segment_seconds = 0;
		uint64_t segment_ms = 0;
		unsigned fraction_digits = 0;

		for (const char *q = p; q != end; ++q) {
			if (*q == '.' || *q == ',') {
				if (seen_fraction || colon != nullptr)
					return 0;
				seen_fraction = true;
				continue;
			}

			if (*q < '0' || *q > '9')
				return 0;

			seen_digit = true;
			const unsigned digit = static_cast<unsigned>(*q - '0');
			if (!seen_fraction) {
				segment_seconds = segment_seconds * 10 + digit;
			} else if (fraction_digits < 3) {
				segment_ms = segment_ms * 10 + digit;
				++fraction_digits;
			}
		}

		if (!seen_digit)
			return 0;

		while (fraction_digits < 3) {
			segment_ms *= 10;
			++fraction_digits;
		}

		total_seconds = total_seconds * 60 + segment_seconds;

		if (colon == nullptr) {
			const uint64_t total_ms = total_seconds * 1000 + segment_ms;
			return total_ms > UINT32_MAX
				? UINT32_MAX
				: static_cast<uint32_t>(total_ms);
		}

		p = colon + 1;
	}
}

[[gnu::pure]]
static unsigned
GetUpseDurationMS(const upse_psf_t &meta) noexcept
{
	unsigned length_ms = 0;
	unsigned fade_ms = 0;

	if (meta.xsf != nullptr) {
		length_ms = ParsePsfTimeMS(meta.xsf->inf_length);
		fade_ms = ParsePsfTimeMS(meta.xsf->inf_fade);
	}

	if (length_ms > 0)
		return length_ms + fade_ms;

	if (meta.length > 0)
		return meta.length;

	if (meta.stop > 0)
		return meta.stop + meta.fade;

	return 0;
}

[[gnu::pure]]
static bool
IsUsefulUpseTag(const char *value) noexcept
{
	if (value == nullptr || StringIsEmpty(value))
		return false;

	const std::string_view v{value};
	return !StringIsEqualIgnoreCase(v, "n/a"sv) && v != "-"sv;
}

static void
UpseEmitTag(TagHandler &handler, std::string_view key, TagType type,
	    const char *value) noexcept
{
	if (!IsUsefulUpseTag(value))
		return;

	if (handler.WantPair())
		handler.OnPair(key, value);

	if (handler.WantTag())
		handler.OnTag(type, value);
}

static void
UpseEmitTags(TagHandler &handler, const upse_psf_t &meta) noexcept
{
	UpseEmitTag(handler, "title"sv, TAG_TITLE, meta.title);
	UpseEmitTag(handler, "artist"sv, TAG_ARTIST, meta.artist);
	UpseEmitTag(handler, "game"sv, TAG_ALBUM, meta.game);
	UpseEmitTag(handler, "year"sv, TAG_DATE, meta.year);
	UpseEmitTag(handler, "genre"sv, TAG_GENRE, meta.genre);
	UpseEmitTag(handler, "comment"sv, TAG_COMMENT, meta.comment);

	if (handler.WantPair()) {
		if (IsUsefulUpseTag(meta.psfby))
			handler.OnPair("psfby"sv, meta.psfby);
		if (IsUsefulUpseTag(meta.copyright))
			handler.OnPair("copyright"sv, meta.copyright);
	}

	if (!IsUsefulUpseTag(meta.artist) && IsUsefulUpseTag(meta.game) &&
	    handler.WantTag())
		handler.OnTag(TAG_ARTIST, meta.game);
}

static bool
upse_plugin_init([[maybe_unused]] const ConfigBlock &block)
{
	if (!upse_initialized) {
		upse_module_init();
		upse_initialized = true;
	}

	return true;
}

static bool
upse_scan_file(Path path_fs, TagHandler &handler) noexcept
{
	const std::string path_utf8 = path_fs.ToUTF8();

	upse_psf_t *meta = upse_get_psf_metadata(path_utf8.c_str(), &upse_io);
	if (meta == nullptr)
		return false;

	AtScopeExit(meta) {
		upse_free_psf_metadata(meta);
	};

	if (handler.WantDuration()) {
		const unsigned duration_ms = GetUpseDurationMS(*meta);
		if (duration_ms > 0)
			handler.OnDuration(SongTime::FromMS(duration_ms));
	}

	if (handler.WantTag() || handler.WantPair())
		UpseEmitTags(handler, *meta);

	return true;
}

static void
upse_file_decode(DecoderClient &client, Path path_fs)
{
	const std::string path_utf8 = path_fs.ToUTF8();

	if (!upse_initialized) {
		upse_module_init();
		upse_initialized = true;
	}

	upse_module_t *mod = upse_module_open(path_utf8.c_str(), &upse_io);
	if (mod == nullptr) {
		LogWarning(upse_domain, "error loading file");
		return;
	}

	AtScopeExit(mod) {
		upse_eventloop_stop(mod);
		upse_module_close(mod);
	};

	const upse_psf_t *meta = mod->metadata;
	const uint32_t sample_rate = meta != nullptr && meta->rate != 0
		? meta->rate
		: UPSE_SAMPLE_RATE;

	if (meta != nullptr && meta->volume == 0 && meta->xsf != nullptr) {
		const std::string_view v{meta->xsf->inf_volume};
		if (v.empty() || StringIsEqualIgnoreCase(v, "n/a"sv))
			upse_ps1_spu_setvolume(mod->instance.spu, 32);
	}

	const SignedSongTime song_len = meta != nullptr
		? SignedSongTime::FromMS(GetUpseDurationMS(*meta))
		: SignedSongTime::Negative();

	const auto audio_format = CheckAudioFormat(sample_rate,
		SampleFormat::S16, UPSE_CHANNELS);

	client.Ready(audio_format, true, song_len);

	const bool has_length = meta != nullptr &&
		GetUpseDurationMS(*meta) > 0;
	const int64_t length_frames = has_length
		? static_cast<int64_t>(GetUpseDurationMS(*meta)) *
			sample_rate / 1000
		: 0;
	int64_t frames_played = 0;

	DecoderCommand cmd = DecoderCommand::NONE;

	do {
		int16_t *samples = nullptr;
		const int frames = upse_eventloop_render(mod, &samples);
		if (frames <= 0)
			break;

		if (samples == nullptr)
			continue;

		cmd = client.SubmitAudio(nullptr,
			std::span{samples, static_cast<size_t>(frames * UPSE_CHANNELS)},
			0);

		frames_played += frames;
		if (has_length && frames_played >= length_frames)
			break;

		if (cmd == DecoderCommand::SEEK) {
			int64_t seek_ms = client.GetSeekTime().ToMS();
			if (seek_ms < 0)
				seek_ms = 0;

			upse_eventloop_seek(mod, static_cast<uint32_t>(seek_ms));
			if (has_length)
				frames_played = seek_ms * sample_rate / 1000;
			client.CommandFinished();
		}
	} while (cmd != DecoderCommand::STOP);
}

static const char *const upse_suffixes[] = {
	"psf",
	"minipsf",
	nullptr
};

constexpr DecoderPlugin upse_decoder_plugin =
	DecoderPlugin("upse", upse_file_decode, upse_scan_file)
	.WithInit(upse_plugin_init)
	.WithSuffixes(upse_suffixes);
