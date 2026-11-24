/******************************************************************************
    CTD OBS NDI module: the raw outputs that feed senders (docs/ctd/ndi.md).

      ctd_ndi_output         OBS_OUTPUT_AV, for the program ("CTD OBS
                             Program") and the vertical canvas ("CTD OBS
                             Vertical"); creates its own sender (settings
                             "ndi_name") and sends video and audio
      ctd_ndi_source_output  OBS_OUTPUT_VIDEO, on a per-source sender's
                             private canvas; sends video into the sender its
                             NDI Output filter made (settings "sender_key");
                             the filter sends the source's own audio

    No encoder, no render callback, nothing on the main window. Formats NDI
    takes (NV12, I420, UYVY, BGRA, BGRX, RGBA) go as they are; OBS converts
    anything else to NV12 for the output.

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-sender.hpp"

#include <media-io/audio-io.h>

#include <map>

namespace ctd_ndi {

/* ------------------------------------------------------------------------------------------------ sender registry */

static std::mutex registryMutex;
static std::map<std::string, std::shared_ptr<Sender>> registry;

void SenderRegistryPut(const std::string &key, std::shared_ptr<Sender> sender)
{
	std::lock_guard<std::mutex> lock(registryMutex);
	registry[key] = std::move(sender);
}

void SenderRegistryRemove(const std::string &key)
{
	std::shared_ptr<Sender> gone;
	{
		std::lock_guard<std::mutex> lock(registryMutex);
		auto it = registry.find(key);
		if (it != registry.end()) {
			gone = std::move(it->second);
			registry.erase(it);
		}
	}
	/* gone is released outside the lock */
}

std::shared_ptr<Sender> SenderRegistryGet(const std::string &key)
{
	std::lock_guard<std::mutex> lock(registryMutex);
	auto it = registry.find(key);
	return it == registry.end() ? nullptr : it->second;
}

/* ------------------------------------------------------------------------------------------------ the outputs */

struct NdiOutput {
	obs_output_t *output = nullptr;
	bool perSource = false;
	std::string ndiName;   /* ctd_ndi_output */
	std::string senderKey; /* ctd_ndi_source_output */
	std::shared_ptr<Sender> sender;
	std::atomic<bool> capturing{false};
};

static const char *ProgramOutputName(void *)
{
	return obs_module_text("NDIOutput");
}

static const char *SourceOutputName(void *)
{
	return obs_module_text("NDIOutput.Source");
}

static void *CreateCommon(obs_data_t *settings, obs_output_t *output, bool perSource)
{
	auto *o = new NdiOutput();
	o->output = output;
	o->perSource = perSource;
	const char *name = obs_data_get_string(settings, "ndi_name");
	o->ndiName = name && *name ? name : "CTD OBS";
	const char *key = obs_data_get_string(settings, "sender_key");
	o->senderKey = key ? key : "";
	return o;
}

static void *ProgramCreate(obs_data_t *settings, obs_output_t *output)
{
	return CreateCommon(settings, output, false);
}

static void *SourceCreate(obs_data_t *settings, obs_output_t *output)
{
	return CreateCommon(settings, output, true);
}

static void OutputStop(void *data, uint64_t ts);

static void OutputDestroy(void *data)
{
	auto *o = static_cast<NdiOutput *>(data);
	if (o->capturing.load() || o->sender) {
		OutputStop(o, 0);
	}
	delete o;
}

static void OutputRawVideo(void *data, video_data *frame)
{
	auto *o = static_cast<NdiOutput *>(data);
	if (o->capturing.load() && o->sender) {
		o->sender->PushVideo(frame);
	}
}

static void OutputRawAudio(void *data, audio_data *frames)
{
	auto *o = static_cast<NdiOutput *>(data);
	if (!o->capturing.load() || !o->sender || !frames) {
		return;
	}
	audio_t *audio = obs_output_audio(o->output);
	const audio_output_info *info = audio ? audio_output_get_info(audio) : nullptr;
	if (!info) {
		return;
	}
	o->sender->SendAudio(frames->data, audio_output_get_channels(audio), frames->frames, info->samples_per_sec);
}

static bool OutputStart(void *data)
{
	auto *o = static_cast<NdiOutput *>(data);
	if (!Running()) {
		obs_output_set_last_error(o->output, "NDI is off");
		return false;
	}
	video_t *video = obs_output_video(o->output);
	const video_output_info *voi = video ? video_output_get_info(video) : nullptr;
	if (!voi) {
		CTD_NDI_LOG(LOG_WARNING, "NDI output: no video to send");
		return false;
	}

	std::string error;
	if (o->perSource) {
		o->sender = SenderRegistryGet(o->senderKey);
		if (!o->sender) {
			CTD_NDI_LOG(LOG_WARNING, "NDI source output: its sender is gone");
			return false;
		}
	} else {
		audio_t *audio = obs_output_audio(o->output);
		const audio_output_info *aoi = audio ? audio_output_get_info(audio) : nullptr;
		if (!aoi) {
			CTD_NDI_LOG(LOG_WARNING, "'%s': no audio to send", o->ndiName.c_str());
			return false;
		}
		audio_convert_info audioConversion = {};
		audioConversion.samples_per_sec = aoi->samples_per_sec;
		audioConversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
		audioConversion.speakers = aoi->speakers;
		obs_output_set_audio_conversion(o->output, &audioConversion);

		o->sender = Sender::Create(o->ndiName, error);
		if (!o->sender) {
			CTD_NDI_LOG(LOG_WARNING, "'%s' did not start: %s", o->ndiName.c_str(), error.c_str());
			obs_output_set_last_error(o->output, error.c_str());
			return false;
		}
	}

	video_format format = voi->format;
	switch (format) {
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_I420:
	case VIDEO_FORMAT_UYVY:
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_RGBA:
		break;
	default: {
		/* I444, P010, … : OBS converts to NV12 for this output */
		video_scale_info conversion = {};
		conversion.format = VIDEO_FORMAT_NV12;
		conversion.width = voi->width;
		conversion.height = voi->height;
		conversion.range = voi->range;
		conversion.colorspace = voi->colorspace;
		obs_output_set_video_conversion(o->output, &conversion);
		format = VIDEO_FORMAT_NV12;
		break;
	}
	}

	if (!o->sender->StartVideo(format, voi->width, voi->height, static_cast<int>(voi->fps_num),
				   static_cast<int>(voi->fps_den), error)) {
		CTD_NDI_LOG(LOG_WARNING, "'%s': %s", o->sender->Name().c_str(), error.c_str());
		o->sender.reset();
		return false;
	}

	o->capturing = true;
	if (!obs_output_can_begin_data_capture(o->output, 0) || !obs_output_begin_data_capture(o->output, 0)) {
		CTD_NDI_LOG(LOG_ERROR, "'%s': OBS refused to start the data capture", o->sender->Name().c_str());
		OutputStop(o, 0);
		return false;
	}
	return true;
}

static void OutputStop(void *data, uint64_t)
{
	auto *o = static_cast<NdiOutput *>(data);
	obs_output_end_data_capture(o->output); /* no more raw_video / raw_audio after this */
	o->capturing = false;
	if (o->sender) {
		o->sender->StopVideo();
	}
	/* the program / vertical sender goes with its output; a per-source sender stays with its filter */
	o->sender.reset();
}

void RegisterOutputs()
{
	obs_output_info info = {};
	info.id = "ctd_ndi_output";
	info.flags = OBS_OUTPUT_AV;
	info.get_name = ProgramOutputName;
	info.create = ProgramCreate;
	info.destroy = OutputDestroy;
	info.start = OutputStart;
	info.stop = OutputStop;
	info.raw_video = OutputRawVideo;
	info.raw_audio = OutputRawAudio;
	obs_register_output(&info);

	obs_output_info source = {};
	source.id = "ctd_ndi_source_output";
	source.flags = OBS_OUTPUT_VIDEO;
	source.get_name = SourceOutputName;
	source.create = SourceCreate;
	source.destroy = OutputDestroy;
	source.start = OutputStart;
	source.stop = OutputStop;
	source.raw_video = OutputRawVideo;
	obs_register_output(&source);
}

} // namespace ctd_ndi
