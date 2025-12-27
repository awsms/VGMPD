// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "LazyusfDecoderPlugin.hxx"
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

#if __has_include(<lazyusf/usf.h>)
#include <lazyusf/usf.h>
#elif __has_include(<usf.h>)
#include <usf.h>
#else
#include <lazyusf/usf.h>
#endif
#else
#include <psflib/psflib.h>
#include <lazyusf/usf.h>
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

static constexpr Domain lazyusf_domain("lazyusf");

static constexpr unsigned LAZYUSF_CHANNELS = 2;
static constexpr unsigned LAZYUSF_BUFFER_FRAMES = 1024;
static constexpr unsigned LAZYUSF_BUFFER_SAMPLES =
	LAZYUSF_BUFFER_FRAMES * LAZYUSF_CHANNELS;
static constexpr unsigned LAZYUSF_SEEK_CHUNK_FRAMES = 8192;
static constexpr unsigned LAZYUSF_SEEK_CHUNK_SAMPLES =
	LAZYUSF_SEEK_CHUNK_FRAMES * LAZYUSF_CHANNELS;

static constexpr char lazyusf_separators[] = "\\/:|";

static bool enable_hle = true;
static int32_t configured_sample_rate = 0;

struct LazyUSFTagHolder {
	unsigned length_ms = 0;
	unsigned fade_ms = 0;
	bool enable_compare = false;
	bool enable_fifo_full = false;
	TagHandler *handler = nullptr;

	void Reset() noexcept {
		length_ms = 0;
		fade_ms = 0;
		enable_compare = false;
		enable_fifo_full = false;
	}
};

/**
 * Small RAII helper to avoid repeating malloc/usf_shutdown/free boilerplate.
 */
class UsfState {
	usf_state_t *p = nullptr;

public:
	UsfState() {
		p = static_cast<usf_state_t *>(std::malloc(usf_get_state_size()));
	}

	~UsfState() {
		if (p != nullptr) {
			usf_shutdown(p);
			std::free(p);
		}
	}

	UsfState(const UsfState &) = delete;
	UsfState &operator=(const UsfState &) = delete;

	usf_state_t *get() const noexcept { return p; }
	explicit operator bool() const noexcept { return p != nullptr; }
};

[[gnu::pure]]
static int16_t
FadeUSFSample(int16_t sample, int64_t numerator, int64_t denominator) noexcept
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

[[gnu::pure]]
static uint32_t
ParseUSFTimeMS(const char *ts) noexcept
{
	if (ts == nullptr || *ts == 0)
		return 0;

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
			if (*q == '.') {
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
LazyUSF_fopen(void *, const char *path)
{
	return std::fopen(path, "rb");
}

static size_t
LazyUSF_fread(void *buffer, size_t size, size_t count, void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::fread(buffer, size, count, f);
}

static int
LazyUSF_fseek(void *handle, int64_t offset, int whence)
{
	FILE *f = static_cast<FILE *>(handle);
	if (offset > LONG_MAX || offset < LONG_MIN)
		return -1;
	return std::fseek(f, static_cast<long>(offset), whence);
}

static int
LazyUSF_fclose(void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::fclose(f);
}

static long
LazyUSF_ftell(void *handle)
{
	FILE *f = static_cast<FILE *>(handle);
	return std::ftell(f);
}

static psf_file_callbacks LazyUSF_psf_callbacks = {
	lazyusf_separators,
	nullptr,
	LazyUSF_fopen,
	LazyUSF_fread,
	LazyUSF_fseek,
	LazyUSF_fclose,
	LazyUSF_ftell,
};

static int
LazyUSF_TagHandler(void *context, const char *name, const char *value)
{
	auto &holder = *static_cast<LazyUSFTagHolder *>(context);
	const std::string_view name_view{name != nullptr ? name : ""};
	const std::string_view value_view{value != nullptr ? value : ""};

	if (holder.handler != nullptr) {
		if (holder.handler->WantPair())
			holder.handler->OnPair(name_view, value_view);

		if (holder.handler->WantTag()) {
			struct Mapping {
				std::string_view key;
				TagType tag;
			};

			static constexpr Mapping map[] = {
				{"title"sv, TAG_TITLE},
				{"artist"sv, TAG_ARTIST},
				{"game"sv, TAG_ALBUM},
				{"year"sv, TAG_DATE},
				{"track"sv, TAG_TRACK},
			};

			for (const auto &m : map) {
				if (StringIsEqualIgnoreCase(name_view, m.key)) {
					holder.handler->OnTag(m.tag, value_view);
					break;
				}
			}
		}
	}

	if (StringIsEqualIgnoreCase(name_view, "length"sv))
		holder.length_ms = ParseUSFTimeMS(value);
	else if (StringIsEqualIgnoreCase(name_view, "fade"sv))
		holder.fade_ms = ParseUSFTimeMS(value);
	else if (StringIsEqualIgnoreCase(name_view, "_enablecompare"sv) &&
		 value != nullptr && !StringIsEmpty(value))
		holder.enable_compare = true;
	else if (StringIsEqualIgnoreCase(name_view, "_enablefifofull"sv) &&
		 value != nullptr && !StringIsEmpty(value))
		holder.enable_fifo_full = true;

	return 0;
}

static int
LazyUSF_Loader(void *context, const uint8_t *, size_t,
	       const uint8_t *reserved, size_t reserved_size)
{
	usf_state_t *usf = static_cast<usf_state_t *>(context);
	return usf_upload_section(usf, reserved, reserved_size);
}

static bool
LazyUSF_openfile(usf_state_t *usf, Path path_fs, LazyUSFTagHolder &holder)
{
	usf_clear(usf);
	holder.Reset();

	const std::string path_utf8 = path_fs.ToUTF8();
	if (psf_load(path_utf8.c_str(),
		     &LazyUSF_psf_callbacks, 0x21,
		     LazyUSF_Loader, usf,
		     LazyUSF_TagHandler, &holder, 1,
		     nullptr, nullptr) < 0) {
		LogWarning(lazyusf_domain, "error loading file");
		return false;
	}

	usf_set_compare(usf, holder.enable_compare);
	usf_set_fifo_full(usf, holder.enable_fifo_full);
	usf_set_hle_audio(usf, enable_hle);

	if (holder.handler != nullptr && holder.handler->WantDuration() &&
	    holder.length_ms > 0) {
		holder.handler->OnDuration(
			SongTime::FromMS(holder.length_ms + holder.fade_ms));
	}

	return true;
}

static void
ApplyFade(int16_t *buf, int64_t frames, int channels, int64_t start_frame,
	  int64_t fade_remaining, int64_t fade_total) noexcept
{
	if (start_frame >= frames)
		return;

	for (int64_t i = start_frame; i < frames; ++i) {
		const int64_t offset = i - start_frame;
		const int64_t remaining = fade_remaining - offset;

		for (int c = 0; c < channels; ++c) {
			auto &s = buf[i * channels + c];

			if (fade_total <= 0 || remaining <= 0) {
				s = 0;
			} else {
				s = FadeUSFSample(s, remaining, fade_total);
			}
		}
	}
}

/**
 * Unified render wrapper:
 * - If resample=false, usf_render() may accept nullptr output (used for skipping).
 * - If resample=true, caller must provide a valid output buffer.
 *
 * Returns nullptr on success, or error string on failure.
 */
static const char *
Render(usf_state_t *usf, bool resample, int16_t *dst,
       size_t frames, int32_t &rate) noexcept
{
	if (resample)
		return usf_render_resampled(usf, dst, frames, rate);

	return usf_render(usf, dst, frames, &rate);
}

static bool
lazyusf_plugin_init(const ConfigBlock &block)
{
	enable_hle = block.GetBlockValue("hle", true);
	configured_sample_rate = block.GetBlockValue("sample_rate", 0);
	if (configured_sample_rate < 0)
		configured_sample_rate = 0;
	return true;
}

static bool
lazyusf_scan_file(Path path_fs, TagHandler &handler) noexcept
{
	LazyUSFTagHolder holder;
	holder.handler = &handler;

	UsfState usf;
	if (!usf) {
		LogWarning(lazyusf_domain, "out of memory");
		return false;
	}

	return LazyUSF_openfile(usf.get(), path_fs, holder);
}

/**
 * Skip forward by "frames" frames by rendering and discarding output.
 * For resample=true, a scratch buffer is required.
 */
static bool
SkipFrames(usf_state_t *usf, bool resample, int32_t &rate,
	   int64_t frames, int16_t *scratch, size_t scratch_frames) noexcept
{
	while (frames > 0) {
		const int64_t chunk = std::min<int64_t>(frames, static_cast<int64_t>(scratch_frames));
		const char *err = Render(usf, resample,
					 resample ? scratch : nullptr,
					 static_cast<size_t>(chunk),
					 rate);
		if (err != nullptr) {
			LogWarning(lazyusf_domain, err);
			return false;
		}

		frames -= chunk;
	}

	return true;
}

static void
lazyusf_file_decode(DecoderClient &client, Path path_fs)
{
	LazyUSFTagHolder holder;

	UsfState usf;
	if (!usf) {
		LogWarning(lazyusf_domain, "out of memory");
		return;
	}

	if (!LazyUSF_openfile(usf.get(), path_fs, holder))
		return;

	int32_t render_rate = configured_sample_rate;
	const bool resample = render_rate > 0;
	const char *usf_err = nullptr;

	/* If we are not resampling, probe/render once to retrieve the native rate. */
	if (!resample) {
		usf_err = Render(usf.get(), false, nullptr, 0, render_rate);
		if (usf_err != nullptr) {
			LogWarning(lazyusf_domain, usf_err);
			return;
		}
	}

	if (render_rate <= 0) {
		LogWarning(lazyusf_domain, "invalid sample rate");
		return;
	}

	const bool has_length = holder.length_ms > 0;
	const int64_t length_frames = has_length
		? static_cast<int64_t>(holder.length_ms) * render_rate / 1000
		: 0;
	const int64_t fade_total = static_cast<int64_t>(holder.fade_ms) *
		render_rate / 1000;

	const SignedSongTime song_len = has_length
		? SignedSongTime::FromMS(holder.length_ms + holder.fade_ms)
		: SignedSongTime::Negative();

	const auto audio_format = CheckAudioFormat(render_rate,
		SampleFormat::S16, LAZYUSF_CHANNELS);

	client.Ready(audio_format, has_length, song_len);

	DecoderCommand cmd = DecoderCommand::NONE;
	int16_t buf[LAZYUSF_BUFFER_SAMPLES];
	int16_t seek_buf[LAZYUSF_SEEK_CHUNK_SAMPLES];

	int64_t song_remaining = length_frames;
	int64_t fade_remaining = fade_total;

	do {
		usf_err = Render(usf.get(), resample, buf,
				 LAZYUSF_BUFFER_FRAMES, render_rate);
		if (usf_err != nullptr) {
			LogWarning(lazyusf_domain, usf_err);
			return;
		}

		if (has_length) {
			const int64_t remaining_before = song_remaining;

			if (song_remaining > 0)
				song_remaining -= LAZYUSF_BUFFER_FRAMES;

			/* Apply fade once we reach/overrun the end of the song body. */
			if (remaining_before <= LAZYUSF_BUFFER_FRAMES) {
				const int64_t fade_start =
					std::max<int64_t>(remaining_before, 0);
				ApplyFade(buf, LAZYUSF_BUFFER_FRAMES, LAZYUSF_CHANNELS,
					  fade_start, fade_remaining, fade_total);

				fade_remaining -= (LAZYUSF_BUFFER_FRAMES - fade_start);
				if (fade_remaining < 0)
					fade_remaining = 0;
			}
		}

		cmd = client.SubmitAudio(nullptr, std::span{buf}, 0);

		if (has_length && song_remaining <= 0 && fade_remaining <= 0)
			break;

		if (cmd == DecoderCommand::SEEK) {
			const int64_t seek_ms = client.GetSeekTime().ToMS();
			const int64_t seek_frames = seek_ms * render_rate / 1000;

			usf_restart(usf.get());
			song_remaining = length_frames;
			fade_remaining = fade_total;

			if (has_length) {
				/* We keep original semantics: seek can extend into fade. */
				const int64_t target_remaining = length_frames - seek_frames;

				/* Skip up to the target position (into or past the body). */
				const int64_t to_skip = std::max<int64_t>(song_remaining - target_remaining, 0);
				if (!SkipFrames(usf.get(), resample, render_rate, to_skip,
						seek_buf, LAZYUSF_SEEK_CHUNK_FRAMES))
					return;

				/* Update remaining counters similarly to the original loop. */
				if (song_remaining > 0) {
					const int64_t skipped_in_body = std::min<int64_t>(to_skip, song_remaining);
					song_remaining -= skipped_in_body;
				}

				if (target_remaining < 0) {
					fade_remaining = std::max<int64_t>(fade_total + target_remaining, 0);
					song_remaining = 0;
				}
			} else {
				if (!SkipFrames(usf.get(), resample, render_rate, seek_frames,
						seek_buf, LAZYUSF_SEEK_CHUNK_FRAMES))
					return;
			}

			client.CommandFinished();
		}
	} while (cmd != DecoderCommand::STOP);
}

static const char *const lazyusf_suffixes[] = {
	"miniusf",
	nullptr
};

constexpr DecoderPlugin lazyusf_decoder_plugin =
	DecoderPlugin("lazyusf", lazyusf_file_decode, lazyusf_scan_file)
	.WithInit(lazyusf_plugin_init)
	.WithSuffixes(lazyusf_suffixes);
