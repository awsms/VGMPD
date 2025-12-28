// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "AopsfDecoderPlugin.hxx"
#include "../DecoderAPI.hxx"
#include "Log.hxx"
#include "pcm/CheckAudioFormat.hxx"
#include "tag/Handler.hxx"
#include "fs/Path.hxx"
#include "util/Domain.hxx"
#include "util/ScopeExit.hxx"
#include "util/StringCompare.hxx"

extern "C" {
#if defined(__has_include)
#if __has_include(<psf2fs.h>)
#include <psf2fs.h>
#elif __has_include(<psflib/psf2fs.h>)
#include <psflib/psf2fs.h>
#else
#include <psf2fs.h>
#endif

#if __has_include(<psflib.h>)
#include <psflib.h>
#elif __has_include(<psflib/psflib.h>)
#include <psflib/psflib.h>
#else
#include <psflib.h>
#endif
#else
#include <psf2fs.h>
#include <psflib.h>
#endif

#include <aopsf/psx_external.h>
}

#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include <string_view>

using std::string_view_literals::operator""sv;

static constexpr Domain aopsf_domain("aopsf");

static constexpr unsigned AOPSF_CHANNELS = 2;
static constexpr unsigned AOPSF_BUFFER_FRAMES = 1024;
static constexpr unsigned AOPSF_SEEK_CHUNK_FRAMES = 8192;

static constexpr unsigned PSF1_SAMPLE_RATE = 44100;
static constexpr unsigned PSF2_SAMPLE_RATE = 48000;

static constexpr char psf_separators[] = "\\/:";

struct AopsfTags {
	unsigned length_ms = 0;
	unsigned fade_ms = 0;
	std::string title;
	std::string artist;
	std::string game;
	std::string year;
	std::string genre;
	std::string comment;
	std::string track;
	std::string psfby;
	std::string copyright;
};

struct AopsfInfoContext {
	AopsfTags tags;
	PSX_STATE *psx = nullptr;
};

struct AopsfLoadContext {
	PSX_STATE *psx = nullptr;
	bool first = true;
};

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
GetPsfDurationMS(const AopsfTags &tags) noexcept
{
	if (tags.length_ms > 0)
		return tags.length_ms + tags.fade_ms;

	return 0;
}

[[gnu::pure]]
static bool
IsUsefulTag(std::string_view value) noexcept
{
	if (value.empty())
		return false;

	return !StringIsEqualIgnoreCase(value, "n/a"sv) && value != "-"sv;
}

static void
EmitTag(TagHandler &handler, std::string_view key, TagType type,
	const std::string &value) noexcept
{
	if (!IsUsefulTag(value))
		return;

	if (handler.WantPair())
		handler.OnPair(key, value);

	if (handler.WantTag())
		handler.OnTag(type, value);
}

static void
EmitTags(TagHandler &handler, const AopsfTags &tags) noexcept
{
	EmitTag(handler, "title"sv, TAG_TITLE, tags.title);
	EmitTag(handler, "artist"sv, TAG_ARTIST, tags.artist);
	EmitTag(handler, "game"sv, TAG_ALBUM, tags.game);
	EmitTag(handler, "year"sv, TAG_DATE, tags.year);
	EmitTag(handler, "genre"sv, TAG_GENRE, tags.genre);
	EmitTag(handler, "comment"sv, TAG_COMMENT, tags.comment);
	EmitTag(handler, "track"sv, TAG_TRACK, tags.track);

	if (handler.WantPair()) {
		if (IsUsefulTag(tags.psfby))
			handler.OnPair("psfby"sv, tags.psfby);
		if (IsUsefulTag(tags.copyright))
			handler.OnPair("copyright"sv, tags.copyright);
	}

	if (!IsUsefulTag(tags.artist) && IsUsefulTag(tags.game) &&
	    handler.WantTag())
		handler.OnTag(TAG_ARTIST, tags.game);
}

static void *
Aopsf_fopen(void *, const char *path)
{
	return std::fopen(path, "rb");
}

static size_t
Aopsf_fread(void *buffer, size_t size, size_t count, void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::fread(buffer, size, count, f);
}

static int
Aopsf_fseek(void *handle, int64_t offset, int whence)
{
	FILE *f = static_cast<FILE *>(handle);
	if (offset > LONG_MAX || offset < LONG_MIN)
		return -1;
	return std::fseek(f, static_cast<long>(offset), whence);
}

static int
Aopsf_fclose(void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::fclose(f);
}

static long
Aopsf_ftell(void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::ftell(f);
}

static psf_file_callbacks Aopsf_psf_callbacks = {
	psf_separators,
	nullptr,
	Aopsf_fopen,
	Aopsf_fread,
	Aopsf_fseek,
	Aopsf_fclose,
	Aopsf_ftell,
};

static int
Aopsf_Info(void *context, const char *name, const char *value)
{
	if (context == nullptr || name == nullptr || value == nullptr)
		return 0;

	auto &ctx = *static_cast<AopsfInfoContext *>(context);
	const std::string_view key{name};

	if (StringIsEqualIgnoreCase(key, "length"sv)) {
		if (ctx.tags.length_ms == 0)
			ctx.tags.length_ms = ParsePsfTimeMS(value);
	} else if (StringIsEqualIgnoreCase(key, "fade"sv)) {
		if (ctx.tags.fade_ms == 0)
			ctx.tags.fade_ms = ParsePsfTimeMS(value);
	} else if (StringIsEqualIgnoreCase(key, "title"sv)) {
		ctx.tags.title = value;
	} else if (StringIsEqualIgnoreCase(key, "artist"sv)) {
		ctx.tags.artist = value;
	} else if (StringIsEqualIgnoreCase(key, "game"sv)) {
		ctx.tags.game = value;
	} else if (StringIsEqualIgnoreCase(key, "year"sv)) {
		ctx.tags.year = value;
	} else if (StringIsEqualIgnoreCase(key, "genre"sv)) {
		ctx.tags.genre = value;
	} else if (StringIsEqualIgnoreCase(key, "comment"sv)) {
		ctx.tags.comment = value;
	} else if (StringIsEqualIgnoreCase(key, "track"sv)) {
		ctx.tags.track = value;
	} else if (StringIsEqualIgnoreCase(key, "psfby"sv)) {
		ctx.tags.psfby = value;
	} else if (StringIsEqualIgnoreCase(key, "copyright"sv)) {
		ctx.tags.copyright = value;
	} else if (StringIsEqualIgnoreCase(key, "_refresh"sv)) {
		if (ctx.psx != nullptr) {
			char *end = nullptr;
			unsigned long refresh = std::strtoul(value, &end, 10);
			if (end != value && *end == 0)
				psx_set_refresh(ctx.psx, refresh);
		}
	}

	return 0;
}

static int
Aopsf_LoadPsf1(void *context, const uint8_t *exe, size_t exe_size,
	      const uint8_t *reserved, size_t reserved_size)
{
	auto &load = *static_cast<AopsfLoadContext *>(context);

	if (load.psx == nullptr)
		return -1;

	if (reserved != nullptr && reserved_size > 0)
		return -1;

	const uint32_t result = psf_load_section(load.psx, exe, exe_size,
		load.first ? 1u : 0u);
	if (result != 0)
		return -1;

	load.first = false;
	return 0;
}

static bool
AopsfSeek(PSX_STATE *psx, uint32_t version, int64_t seek_ms,
	  unsigned sample_rate) noexcept
{
	if (seek_ms < 0)
		seek_ms = 0;

	const uint32_t restart = version == 2
		? psf2_command(psx, COMMAND_RESTART, 0)
		: psf_command(psx, COMMAND_RESTART, 0);
	if (restart != AO_SUCCESS)
		return false;

	const int64_t target_frames = seek_ms * sample_rate / 1000;
	if (target_frames <= 0)
		return true;

	std::array<int16_t, AOPSF_SEEK_CHUNK_FRAMES * AOPSF_CHANNELS> buffer{};
	int64_t frames_skipped = 0;
	while (frames_skipped < target_frames) {
		const int64_t remaining = target_frames - frames_skipped;
		const uint32_t chunk = static_cast<uint32_t>(
			std::min<int64_t>(remaining, AOPSF_SEEK_CHUNK_FRAMES));
		const uint32_t result = version == 2
			? psf2_gen(psx, buffer.data(), chunk)
			: psf_gen(psx, buffer.data(), chunk);
		if (result != AO_SUCCESS)
			return false;

		frames_skipped += chunk;
	}

	return true;
}

static bool
aopsf_scan_file(Path path_fs, TagHandler &handler) noexcept
{
	const std::string path_utf8 = path_fs.ToUTF8();

	AopsfInfoContext info_ctx;
	const int version = psf_load(path_utf8.c_str(), &Aopsf_psf_callbacks, 0,
		nullptr, nullptr, &Aopsf_Info, &info_ctx, 1, nullptr, nullptr);
	if (version <= 0)
		return false;

	if (handler.WantDuration()) {
		const unsigned duration_ms = GetPsfDurationMS(info_ctx.tags);
		if (duration_ms > 0)
			handler.OnDuration(SongTime::FromMS(duration_ms));
	}

	if (handler.WantTag() || handler.WantPair())
		EmitTags(handler, info_ctx.tags);

	return true;
}

static void
aopsf_file_decode(DecoderClient &client, Path path_fs)
{
	const std::string path_utf8 = path_fs.ToUTF8();

	AopsfInfoContext info_ctx;
	const int version = psf_load(path_utf8.c_str(), &Aopsf_psf_callbacks, 0,
		nullptr, nullptr, &Aopsf_Info, &info_ctx, 1, nullptr, nullptr);
	if (version <= 0) {
		LogWarning(aopsf_domain, "error probing file");
		return;
	}

	const size_t psx_size = psx_get_state_size(version);
	auto *psx_storage = static_cast<unsigned char *>(std::malloc(psx_size));
	if (psx_storage == nullptr) {
		LogWarning(aopsf_domain, "allocation failed");
		return;
	}
	std::memset(psx_storage, 0, psx_size);
	auto *psx = reinterpret_cast<PSX_STATE *>(psx_storage);

	void *psf2fs = nullptr;
	bool started = false;

	AtScopeExit(&) {
		if (started) {
			if (version == 2)
				psf2_stop(psx);
			else
				psf_stop(psx);
		}

		if (psf2fs != nullptr)
			psf2fs_delete(psf2fs);

		std::free(psx_storage);
	};

	info_ctx.psx = psx;

	if (version == 1) {
		AopsfLoadContext load_ctx{psx, true};
		const int ret = psf_load(path_utf8.c_str(), &Aopsf_psf_callbacks, 1,
			&Aopsf_LoadPsf1, &load_ctx, &Aopsf_Info, &info_ctx,
			1, nullptr, nullptr);
		if (ret != 1) {
			LogWarning(aopsf_domain, "invalid PSF file");
			return;
		}

		if (psf_start(psx) != AO_SUCCESS) {
			LogWarning(aopsf_domain, "PSF start failed");
			return;
		}
	} else if (version == 2) {
		psf2fs = psf2fs_create();
		if (psf2fs == nullptr) {
			LogWarning(aopsf_domain, "PSF2 filesystem init failed");
			return;
		}

		const int ret = psf_load(path_utf8.c_str(), &Aopsf_psf_callbacks, 2,
			&psf2fs_load_callback, psf2fs, &Aopsf_Info, &info_ctx,
			1, nullptr, nullptr);
		if (ret != 2) {
			LogWarning(aopsf_domain, "invalid PSF2 file");
			return;
		}

		psf2_register_readfile(psx, &psf2fs_virtual_readfile, psf2fs);
		if (psf2_start(psx) != AO_SUCCESS) {
			LogWarning(aopsf_domain, "PSF2 start failed");
			return;
		}
	} else {
		LogWarning(aopsf_domain, "unsupported PSF version");
		return;
	}

	started = true;

	const unsigned sample_rate = version == 2
		? PSF2_SAMPLE_RATE
		: PSF1_SAMPLE_RATE;

	const unsigned duration_ms = GetPsfDurationMS(info_ctx.tags);
	const bool has_length = duration_ms > 0;
	const SignedSongTime song_len = has_length
		? SignedSongTime::FromMS(duration_ms)
		: SignedSongTime::Negative();

	const auto audio_format = CheckAudioFormat(sample_rate,
		SampleFormat::S16, AOPSF_CHANNELS);

	client.Ready(audio_format, has_length, song_len);

	int64_t frames_played = 0;
	const int64_t length_frames = has_length
		? static_cast<int64_t>(duration_ms) * sample_rate / 1000
		: 0;

	std::array<int16_t, AOPSF_BUFFER_FRAMES * AOPSF_CHANNELS> buffer{};
	DecoderCommand cmd = DecoderCommand::NONE;

	do {
		uint32_t frames = AOPSF_BUFFER_FRAMES;
		if (has_length) {
			const int64_t remaining = length_frames - frames_played;
			if (remaining <= 0)
				break;
			frames = static_cast<uint32_t>(
				std::min<int64_t>(remaining, AOPSF_BUFFER_FRAMES));
		}

		const uint32_t result = version == 2
			? psf2_gen(psx, buffer.data(), frames)
			: psf_gen(psx, buffer.data(), frames);
		if (result != AO_SUCCESS) {
			const char *msg = psx_get_last_error(psx);
			LogWarning(aopsf_domain, msg != nullptr ? msg : "decode error");
			break;
		}

		cmd = client.SubmitAudio(nullptr,
			std::span{buffer.data(),
				static_cast<size_t>(frames * AOPSF_CHANNELS)},
			0);

		frames_played += frames;

		if (cmd == DecoderCommand::SEEK) {
			const int64_t seek_ms = client.GetSeekTime().ToMS();
			if (!AopsfSeek(psx, version, seek_ms, sample_rate)) {
				LogWarning(aopsf_domain, "seek failed");
				cmd = DecoderCommand::STOP;
			} else if (has_length) {
				frames_played = seek_ms * sample_rate / 1000;
			} else {
				frames_played = 0;
			}
			client.CommandFinished();
		}
	} while (cmd != DecoderCommand::STOP);
}

static const char *const aopsf_suffixes[] = {
	"psf2",
	"minipsf2",
	nullptr
};

constexpr DecoderPlugin aopsf_decoder_plugin =
	DecoderPlugin("aopsf", aopsf_file_decode, aopsf_scan_file)
	.WithSuffixes(aopsf_suffixes);
