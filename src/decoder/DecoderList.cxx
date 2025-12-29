// SPDX-License-Identifier: GPL-2.0-or-later
// Copyright The Music Player Daemon Project

#include "config.h" // for ENABLE_FFMPEG
#include "DecoderList.hxx"
#include "DecoderPlugin.hxx"
#include "Domain.hxx"
#include "decoder/Features.h"
#include "pcm/Features.h" // for ENABLE_DSD
#include "lib/fmt/ExceptionFormatter.hxx"
#include "lib/fmt/RuntimeError.hxx"
#include "config/Data.hxx"
#include "config/Block.hxx"
#include "plugins/AudiofileDecoderPlugin.hxx"
#include "plugins/PcmDecoderPlugin.hxx"
#include "plugins/DsdiffDecoderPlugin.hxx"
#include "plugins/DsfDecoderPlugin.hxx"
#include "plugins/FlacDecoderPlugin.h"
#include "plugins/OpusDecoderPlugin.h"
#include "plugins/VorbisDecoderPlugin.h"
#include "plugins/AdPlugDecoderPlugin.h"
#include "plugins/WavpackDecoderPlugin.hxx"
#include "plugins/FfmpegDecoderPlugin.hxx"
#include "plugins/GmeDecoderPlugin.hxx"
#include "plugins/LazygsfDecoderPlugin.hxx"
#include "plugins/LazyusfDecoderPlugin.hxx"
#include "plugins/AopsfDecoderPlugin.hxx"
#include "plugins/UpseDecoderPlugin.hxx"
#include "plugins/VgmstreamDecoderPlugin.hxx"
#include "plugins/FaadDecoderPlugin.hxx"
#include "plugins/MadDecoderPlugin.hxx"
#include "plugins/SndfileDecoderPlugin.hxx"
#include "plugins/Mpg123DecoderPlugin.hxx"
#include "plugins/WildmidiDecoderPlugin.hxx"
#include "plugins/MikmodDecoderPlugin.hxx"
#include "plugins/ModplugDecoderPlugin.hxx"
#include "plugins/OpenmptDecoderPlugin.hxx"
#include "plugins/MpcdecDecoderPlugin.hxx"
#include "plugins/FluidsynthDecoderPlugin.hxx"
#include "plugins/SidplayDecoderPlugin.hxx"
#include "Log.hxx"
#include "PluginUnavailable.hxx"
#include "util/CharUtil.hxx"

#include <algorithm> // for std::any_of()
#include <iterator>
#include <string>
#include <unordered_map>
#include <vector>

#include <string.h>

static std::unordered_map<std::string, std::vector<const DecoderPlugin *>>
decoder_codec_priorities;

static std::vector<std::string>
ParseCodecList(const char *value)
{
	std::vector<std::string> result;
	if (value == nullptr)
		return result;

	std::string_view s{value};
	std::size_t i = 0;

	while (i < s.size()) {
		while (i < s.size() &&
		       (s[i] == ',' || IsWhitespaceFast(s[i])))
			++i;

		if (i >= s.size())
			break;

		const std::size_t start = i;
		while (i < s.size() &&
		       s[i] != ',' &&
		       !IsWhitespaceFast(s[i]))
			++i;

		if (i > start) {
			std::string token;
			token.reserve(i - start);
			for (std::size_t j = start; j < i; ++j)
				token.push_back(ToLowerASCII(s[j]));

			if (!token.empty())
				result.push_back(std::move(token));
		}
	}

	return result;
}

static void
BuildDecoderCodecPriorities(const ConfigData &config) noexcept
{
	decoder_codec_priorities.clear();

	config.WithEach(ConfigBlockOption::DECODER, [&](const ConfigBlock &block){
		const char *plugin_name = block.GetBlockValue("plugin", nullptr);
		if (plugin_name == nullptr)
			return;

		const auto *plugin = decoder_plugin_from_name(plugin_name);
		if (plugin == nullptr) {
			FmtWarning(decoder_domain,
				   "Ignoring decoder codecs override for {:?}: plugin not enabled",
				   plugin_name);
			return;
		}

		const auto codecs = ParseCodecList(block.GetBlockValue("codecs", nullptr));
		if (codecs.empty())
			return;

		for (const auto &codec : codecs) {
			auto &list = decoder_codec_priorities[codec];
			if (std::find(list.begin(), list.end(), plugin) == list.end())
				list.push_back(plugin);
		}
	});
}

constinit const struct DecoderPlugin *const decoder_plugins[] = {
#ifdef ENABLE_MPG123
	&mpg123_decoder_plugin,
#endif
#ifdef ENABLE_MAD
	&mad_decoder_plugin,
#endif
#ifdef ENABLE_VORBIS_DECODER
	&vorbis_decoder_plugin,
#endif
#ifdef ENABLE_FLAC
	&oggflac_decoder_plugin,
	&flac_decoder_plugin,
#endif
#ifdef ENABLE_OPUS
	&opus_decoder_plugin,
#endif
#ifdef ENABLE_DSD
	&dsdiff_decoder_plugin,
	&dsf_decoder_plugin,
#endif
#ifdef ENABLE_FAAD
	&faad_decoder_plugin,
#endif
#ifdef ENABLE_MPCDEC
	&mpcdec_decoder_plugin,
#endif
#ifdef ENABLE_WAVPACK
	&wavpack_decoder_plugin,
#endif
#ifdef ENABLE_OPENMPT
	&openmpt_decoder_plugin,
#endif
#ifdef ENABLE_MODPLUG
	&modplug_decoder_plugin,
#endif
#ifdef ENABLE_LIBMIKMOD
	&mikmod_decoder_plugin,
#endif
#ifdef ENABLE_SIDPLAY
	&sidplay_decoder_plugin,
#endif
#ifdef ENABLE_WILDMIDI
	&wildmidi_decoder_plugin,
#endif
#ifdef ENABLE_FLUIDSYNTH
	&fluidsynth_decoder_plugin,
#endif
#ifdef ENABLE_ADPLUG
	&adplug_decoder_plugin,
#endif
#ifdef ENABLE_GME
	&gme_decoder_plugin,
#endif
#ifdef ENABLE_LAZYUSF
	&lazyusf_decoder_plugin,
#endif
#ifdef ENABLE_LAZYGSF
	&gsf_decoder_plugin,
#endif
#ifdef ENABLE_UPSE
	&upse_decoder_plugin,
#endif
#ifdef ENABLE_AOPSF
	&aopsf_decoder_plugin,
#endif
#ifdef ENABLE_VGMSTREAM
	&vgmstream_decoder_plugin,
#endif
#ifdef ENABLE_FFMPEG
	&ffmpeg_decoder_plugin,
#endif

	/* these WAV-decoding plugins are below ffmpeg_decoder_plugin
	   to give FFmpeg a chance to decode DTS-WAV files which is
	   technically DTS Coherent Acoustics (DCA) stream wrapped in
	   fake 16-bit stereo samples; neither libsndfile nor
	   libaudiofile detect this, but FFmpeg does */
#ifdef ENABLE_SNDFILE
	&sndfile_decoder_plugin,
#endif
#ifdef ENABLE_AUDIOFILE
	&audiofile_decoder_plugin,
#endif

	&pcm_decoder_plugin,
	nullptr
};

static constexpr unsigned num_decoder_plugins =
	std::size(decoder_plugins) - 1;

/** which plugins have been initialized successfully? */
bool decoder_plugins_enabled[num_decoder_plugins];

const struct DecoderPlugin *
decoder_plugin_from_name(const char *name) noexcept
{
	return decoder_plugins_find([=](const DecoderPlugin &plugin){
			return strcmp(plugin.name, name) == 0;
		});
}

void
decoder_plugin_init_all(const ConfigData &config)
{
	ConfigBlock empty;

	for (unsigned i = 0; decoder_plugins[i] != nullptr; ++i) {
		const DecoderPlugin &plugin = *decoder_plugins[i];
		const auto *param =
			config.FindBlock(ConfigBlockOption::DECODER, "plugin",
					 plugin.name);

		if (param == nullptr)
			param = &empty;
		else if (!param->GetBlockValue("enabled", true))
			/* the plugin is disabled in mpd.conf */
			continue;

		if (param != nullptr)
			param->SetUsed();

		try {
			if (plugin.Init(*param))
				decoder_plugins_enabled[i] = true;
		} catch (const PluginUnavailable &e) {
			FmtError(decoder_domain,
				 "Decoder plugin {:?} is unavailable: {}",
				 plugin.name, std::current_exception());
		} catch (...) {
			std::throw_with_nested(FmtRuntimeError("Failed to initialize decoder plugin {:?}",
							       plugin.name));
		}
	}

	BuildDecoderCodecPriorities(config);
}

void
decoder_plugin_deinit_all() noexcept
{
	for (const auto &plugin : GetEnabledDecoderPlugins())
		plugin.Finish();
}

bool
decoder_plugins_supports_suffix(std::string_view suffix) noexcept
{
	for (const auto &plugin : GetEnabledDecoderPlugins()) {
		if (plugin.SupportsSuffix(suffix))
			return true;
	}

	return false;
}

std::vector<const DecoderPlugin *>
decoder_plugins_for_suffix(std::string_view suffix) noexcept
{
	std::vector<const DecoderPlugin *> result;

	if (!suffix.empty()) {
		std::string key;
		key.reserve(suffix.size());
		for (const char ch : suffix)
			key.push_back(ToLowerASCII(ch));

		const auto i = decoder_codec_priorities.find(key);
		if (i != decoder_codec_priorities.end())
			result.insert(result.end(), i->second.begin(), i->second.end());
	}

	for (const auto &plugin : GetEnabledDecoderPlugins()) {
		const auto *p = &plugin;
		if (std::find(result.begin(), result.end(), p) == result.end())
			result.push_back(p);
	}

	return result;
}
