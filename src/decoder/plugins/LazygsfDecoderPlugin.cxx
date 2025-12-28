// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "LazygsfDecoderPlugin.hxx"
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
#if __has_include(<psflib/psflib.h>)
#include <psflib/psflib.h>
#elif __has_include(<psflib.h>)
#include <psflib.h>
#else
#include <psflib/psflib.h>
#endif

#if __has_include(<lazygsf/lazygsf.h>)
#include <lazygsf/lazygsf.h>
#elif __has_include(<lazygsf.h>)
#include <lazygsf.h>
#else
#include <lazygsf/lazygsf.h>
#endif
#else
#include <psflib/psflib.h>
#include <lazygsf/lazygsf.h>
#endif
}

#include <algorithm>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

using std::string_view_literals::operator""sv;

static constexpr Domain gsf_domain("lazygsf");

static constexpr unsigned GSF_CHANNELS = 2;
static constexpr unsigned GSF_SAMPLE_RATE_DEFAULT = 44100;
static constexpr unsigned GSF_BUFFER_FRAMES = 2048;
static constexpr unsigned GSF_SEEK_CHUNK_FRAMES = 8192;

static bool gsf_initialized = false;
static unsigned configured_sample_rate = GSF_SAMPLE_RATE_DEFAULT;

struct GsfTagHolder {
	unsigned length_ms = 0;
	unsigned fade_ms = 0;
	TagHandler *handler = nullptr;

	void Reset() noexcept {
		length_ms = 0;
		fade_ms = 0;
	}
};

class GsfState {
	gsf_state_t *p = nullptr;

public:
	GsfState() {
		p = static_cast<gsf_state_t *>(std::malloc(gsf_get_state_size()));
		if (p != nullptr)
			gsf_clear(p);
	}

	~GsfState() {
		if (p != nullptr) {
			gsf_shutdown(p);
			std::free(p);
		}
	}

	GsfState(const GsfState &) = delete;
	GsfState &operator=(const GsfState &) = delete;

	gsf_state_t *get() const noexcept { return p; }
	explicit operator bool() const noexcept { return p != nullptr; }
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

static void *
Gsf_fopen(void *, const char *path)
{
	return std::fopen(path, "rb");
}

static size_t
Gsf_fread(void *buffer, size_t size, size_t count, void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::fread(buffer, size, count, f);
}

static int
Gsf_fseek(void *handle, int64_t offset, int whence)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::fseek(f, static_cast<long>(offset), whence);
}

static int
Gsf_fclose(void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::fclose(f);
}

static long
Gsf_ftell(void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::ftell(f);
}

static const psf_file_callbacks gsf_psf_callbacks = {
	"\\/",
	nullptr,
	Gsf_fopen,
	Gsf_fread,
	Gsf_fseek,
	Gsf_fclose,
	Gsf_ftell,
};

static int
GsfLoader(void *context, const uint8_t *exe, size_t exe_size,
	  const uint8_t *, size_t) noexcept
{
	if (exe == nullptr || exe_size == 0)
		return 0;

	gsf_state_t *state = static_cast<gsf_state_t *>(context);
	return gsf_upload_section(state, exe, exe_size);
}

[[gnu::pure]]
static bool
IsUsefulTag(const char *value) noexcept
{
	if (value == nullptr || StringIsEmpty(value))
		return false;

	const std::string_view v{value};
	return !StringIsEqualIgnoreCase(v, "n/a"sv) && v != "-"sv;
}

static void
GsfEmitTag(GsfTagHolder &holder, std::string_view key, TagType type,
	   const char *value) noexcept
{
	if (holder.handler == nullptr || !IsUsefulTag(value))
		return;

	if (holder.handler->WantPair())
		holder.handler->OnPair(key, value);

	if (holder.handler->WantTag())
		holder.handler->OnTag(type, value);
}

static int
GsfInfo(void *context, const char *name, const char *value)
{
	GsfTagHolder &holder = *static_cast<GsfTagHolder *>(context);
	const std::string_view name_view{name};

	if (StringIsEqualIgnoreCase(name_view, "length"sv)) {
		holder.length_ms = ParsePsfTimeMS(value);
		return 0;
	}

	if (StringIsEqualIgnoreCase(name_view, "fade"sv)) {
		holder.fade_ms = ParsePsfTimeMS(value);
		return 0;
	}

	if (StringIsEqualIgnoreCase(name_view, "title"sv))
		GsfEmitTag(holder, "title"sv, TAG_TITLE, value);
	else if (StringIsEqualIgnoreCase(name_view, "artist"sv))
		GsfEmitTag(holder, "artist"sv, TAG_ARTIST, value);
	else if (StringIsEqualIgnoreCase(name_view, "game"sv))
		GsfEmitTag(holder, "game"sv, TAG_ALBUM, value);
	else if (StringIsEqualIgnoreCase(name_view, "year"sv))
		GsfEmitTag(holder, "year"sv, TAG_DATE, value);
	else if (StringIsEqualIgnoreCase(name_view, "genre"sv))
		GsfEmitTag(holder, "genre"sv, TAG_GENRE, value);
	else if (StringIsEqualIgnoreCase(name_view, "comment"sv))
		GsfEmitTag(holder, "comment"sv, TAG_COMMENT, value);
	else if (holder.handler != nullptr && holder.handler->WantPair()) {
		if (StringIsEqualIgnoreCase(name_view, "gsfby"sv))
			holder.handler->OnPair("gsfby"sv, value);
		else if (StringIsEqualIgnoreCase(name_view, "copyright"sv))
			holder.handler->OnPair("copyright"sv, value);
	}

	return 0;
}

static bool
gsf_plugin_init(const ConfigBlock &block)
{
	if (!gsf_initialized) {
		gsf_init();
		gsf_initialized = true;
	}

	configured_sample_rate = block.GetBlockValue("sample_rate",
		GSF_SAMPLE_RATE_DEFAULT);
	if (configured_sample_rate == 0)
		configured_sample_rate = GSF_SAMPLE_RATE_DEFAULT;

	return true;
}

static bool
GsfLoadFile(Path path_fs, GsfTagHolder &holder,
	    gsf_state_t *state) noexcept
{
	holder.Reset();
	const std::string path_utf8 = path_fs.ToUTF8();

	const int rc = psf_load(path_utf8.c_str(), &gsf_psf_callbacks, 0x22,
				state != nullptr ? GsfLoader : nullptr, state,
				GsfInfo, &holder, 1, nullptr, nullptr);
	if (rc < 0) {
		LogWarning(gsf_domain, "error loading file");
		return false;
	}

	if (holder.handler != nullptr && holder.handler->WantDuration() &&
	    holder.length_ms > 0) {
		holder.handler->OnDuration(
			SongTime::FromMS(holder.length_ms + holder.fade_ms));
	}

	return true;
}

static bool
gsf_scan_file(Path path_fs, TagHandler &handler) noexcept
{
	GsfTagHolder holder;
	holder.handler = &handler;

	return GsfLoadFile(path_fs, holder, nullptr);
}

[[gnu::pure]]
static int16_t
FadeSample(int16_t sample, int64_t numerator, int64_t denominator) noexcept
{
	if (sample == 0)
		return 0;

	if (denominator <= 0 || numerator <= 0)
		return 0;

	int64_t scaled = static_cast<int64_t>(sample);
	scaled *= numerator;
	scaled /= denominator;

	if (scaled > INT16_MAX)
		scaled = INT16_MAX;
	else if (scaled < INT16_MIN)
		scaled = INT16_MIN;

	return static_cast<int16_t>(scaled);
}

static void
ApplyFade(int16_t *samples, size_t frames, unsigned channels,
	  int64_t offset_frames, int64_t fade_remaining,
	  int64_t fade_total) noexcept
{
	if (samples == nullptr || channels == 0 || fade_total <= 0)
		return;

	if (fade_remaining < 0)
		fade_remaining = 0;

	for (size_t i = 0; i < frames; ++i) {
		const int64_t pos = offset_frames + static_cast<int64_t>(i);
		const int64_t numerator = fade_remaining - pos;
		for (unsigned c = 0; c < channels; ++c) {
			const size_t idx = i * channels + c;
			samples[idx] = FadeSample(samples[idx], numerator, fade_total);
		}
	}
}

static bool
SkipFrames(gsf_state_t *state, int64_t frames) noexcept
{
	while (frames > 0) {
		const size_t chunk = static_cast<size_t>(std::min<int64_t>(
			frames, GSF_SEEK_CHUNK_FRAMES));
		if (gsf_render(state, nullptr, chunk) != 0)
			return false;
		frames -= static_cast<int64_t>(chunk);
	}

	return true;
}

static void
gsf_file_decode(DecoderClient &client, Path path_fs)
{
	if (!gsf_initialized) {
		gsf_init();
		gsf_initialized = true;
	}

	GsfState state;
	if (!state) {
		LogWarning(gsf_domain, "out of memory");
		return;
	}

	GsfTagHolder holder;
	if (!GsfLoadFile(path_fs, holder, state.get()))
		return;

	const unsigned sample_rate = gsf_set_sample_rate(state.get(),
		configured_sample_rate);

	const bool has_length = holder.length_ms > 0;
	const int64_t length_frames = has_length
		? static_cast<int64_t>(holder.length_ms) *
			sample_rate / 1000
		: 0;
	const int64_t fade_total = static_cast<int64_t>(holder.fade_ms) *
		sample_rate / 1000;

	const SignedSongTime song_len = has_length
		? SignedSongTime::FromMS(holder.length_ms + holder.fade_ms)
		: SignedSongTime::Negative();

	const auto audio_format = CheckAudioFormat(sample_rate,
		SampleFormat::S16, GSF_CHANNELS);

	client.Ready(audio_format, has_length, song_len);

	DecoderCommand cmd = DecoderCommand::NONE;
	int16_t buf[GSF_BUFFER_FRAMES * GSF_CHANNELS];

	int64_t song_remaining = length_frames;
	int64_t fade_remaining = fade_total;

	do {
		if (gsf_render(state.get(), buf, GSF_BUFFER_FRAMES) != 0) {
			LogWarning(gsf_domain, "render error");
			return;
		}

		if (has_length) {
			const int64_t remaining_before = song_remaining;

			if (song_remaining > 0)
				song_remaining -= GSF_BUFFER_FRAMES;

			if (remaining_before <= GSF_BUFFER_FRAMES) {
				const int64_t fade_start =
					std::max<int64_t>(remaining_before, 0);
				ApplyFade(buf, GSF_BUFFER_FRAMES, GSF_CHANNELS,
					  fade_start, fade_remaining, fade_total);

				fade_remaining -= (GSF_BUFFER_FRAMES - fade_start);
				if (fade_remaining < 0)
					fade_remaining = 0;
			}
		}

		cmd = client.SubmitAudio(nullptr, std::span{buf}, 0);

		if (has_length && song_remaining <= 0 && fade_remaining <= 0)
			break;

		if (cmd == DecoderCommand::SEEK) {
			const int64_t seek_ms = client.GetSeekTime().ToMS();
			const int64_t seek_frames = seek_ms * sample_rate / 1000;

			gsf_restart(state.get());
			song_remaining = length_frames;
			fade_remaining = fade_total;

			if (has_length) {
				const int64_t target_remaining = length_frames - seek_frames;
				const int64_t to_skip =
					std::max<int64_t>(song_remaining - target_remaining, 0);

				if (!SkipFrames(state.get(), to_skip)) {
					LogWarning(gsf_domain, "seek failed");
					return;
				}

				if (song_remaining > 0) {
					const int64_t skipped_in_body =
						std::min<int64_t>(to_skip, song_remaining);
					song_remaining -= skipped_in_body;
				}

				if (target_remaining < 0) {
					fade_remaining = std::max<int64_t>(fade_total + target_remaining, 0);
					song_remaining = 0;
				}
			} else {
				if (!SkipFrames(state.get(), seek_frames)) {
					LogWarning(gsf_domain, "seek failed");
					return;
				}
			}

			client.CommandFinished();
		}
	} while (cmd != DecoderCommand::STOP);
}

static const char *const gsf_suffixes[] = {
	"gsf",
	"minigsf",
	nullptr
};

constexpr DecoderPlugin gsf_decoder_plugin =
	DecoderPlugin("lazygsf", gsf_file_decode, gsf_scan_file)
	.WithInit(gsf_plugin_init)
	.WithSuffixes(gsf_suffixes);
