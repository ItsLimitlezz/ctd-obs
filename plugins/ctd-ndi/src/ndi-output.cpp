/******************************************************************************
    CTD OBS NDI module: NDI send (docs/ctd/ndi.md, "Send").

    ctd_ndi_output is an OBS_OUTPUT_AV raw output: no encoder, no render
    callback, nothing on the main window.

      video   raw_video (OBS's video thread) copies the frame into one of 3
              preallocated buffers and pushes it into a BoundedQueue of 2
              (full: the oldest is dropped and its buffer reused); a sender
              thread sends with NDIlib_send_send_video_v2, synchronous, so
              NDI never reads a buffer that is reused. Memory is fixed at 3
              frames whatever NDI or the network does.
      audio   planar float, copied into one reusable buffer and sent with
              NDIlib_send_send_audio_v3

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-module.hpp"

#include <media-io/audio-io.h>
#include <media-io/video-io.h>

#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

namespace ctd_ndi {

constexpr size_t kSendQueue = 2;
constexpr size_t kSendBuffers = 3;

static std::atomic<int> activeSends{0};
static std::atomic<uint64_t> droppedTotal{0};

int ActiveSends()
{
	return activeSends.load();
}

uint64_t DroppedSendFrames()
{
	return droppedTotal.load();
}

struct VideoBuffer {
	std::vector<uint8_t> data;
};

struct NdiOutput {
	obs_output_t *output = nullptr;
	std::string ndiName;

	const NDIlib_v6 *lib = nullptr;
	NDIlib_send_instance_t sender = nullptr;

	/* frame layout, fixed between start and stop */
	NDIlib_FourCC_video_type_e fourcc = NDIlib_FourCC_video_type_NV12;
	video_format format = VIDEO_FORMAT_NV12;
	uint32_t width = 0, height = 0;
	int stride = 0;
	size_t frameBytes = 0;
	int fpsNum = 30, fpsDen = 1;

	std::unique_ptr<BoundedQueue<std::unique_ptr<VideoBuffer>>> queue;
	std::mutex poolMutex;
	std::vector<std::unique_ptr<VideoBuffer>> pool;
	std::thread senderThread;
	std::atomic<bool> running{false};
	std::atomic<uint64_t> dropped{0};
	bool counted = false; /* in activeSends */

	std::vector<float> audioBuffer;
};

static const char *OutputName(void *)
{
	return obs_module_text("NDIOutput");
}

static void *OutputCreate(obs_data_t *settings, obs_output_t *output)
{
	auto *o = new NdiOutput();
	o->output = output;
	const char *name = obs_data_get_string(settings, "ndi_name");
	o->ndiName = name && *name ? name : "CTD OBS";
	return o;
}

static void OutputStop(void *data, uint64_t ts);

static void OutputDestroy(void *data)
{
	auto *o = static_cast<NdiOutput *>(data);
	if (o->running.load()) {
		OutputStop(o, 0);
	}
	delete o;
}

static void ReturnBuffer(NdiOutput *o, std::unique_ptr<VideoBuffer> buffer)
{
	if (!buffer) {
		return;
	}
	std::lock_guard<std::mutex> lock(o->poolMutex);
	o->pool.push_back(std::move(buffer));
}

static void SenderMain(NdiOutput *o)
{
	while (o->running.load()) {
		auto buffer = o->queue->PopWait(std::chrono::milliseconds(100));
		if (!buffer || !*buffer) {
			continue;
		}
		try {
			NDIlib_video_frame_v2_t frame = {};
			frame.xres = static_cast<int>(o->width);
			frame.yres = static_cast<int>(o->height);
			frame.FourCC = o->fourcc;
			frame.frame_rate_N = o->fpsNum;
			frame.frame_rate_D = o->fpsDen;
			frame.picture_aspect_ratio = 0.0f; /* square pixels */
			frame.frame_format_type = NDIlib_frame_format_type_progressive;
			frame.timecode = NDIlib_send_timecode_synthesize;
			frame.p_data = (*buffer)->data.data();
			frame.line_stride_in_bytes = o->stride;
			/* synchronous: NDI is done with the buffer when this returns */
			o->lib->send_send_video_v2(o->sender, &frame);
		} catch (...) {
			CTD_NDI_LOG(LOG_WARNING, "'%s': NDI video send failed", o->ndiName.c_str());
		}
		ReturnBuffer(o, std::move(*buffer));
	}
}

/* Copies a frame into a packed buffer in the layout NDI wants for the FourCC */
static void PackFrame(const NdiOutput *o, const video_data *frame, uint8_t *dst)
{
	auto copyPlane = [&](int plane, uint32_t rowBytes, uint32_t rows, uint8_t *out) {
		const uint8_t *src = frame->data[plane];
		const uint32_t srcStride = frame->linesize[plane];
		for (uint32_t y = 0; y < rows; y++) {
			memcpy(out + size_t(y) * rowBytes, src + size_t(y) * srcStride, rowBytes);
		}
	};
	const uint32_t w = o->width, h = o->height;
	switch (o->format) {
	case VIDEO_FORMAT_NV12:
		copyPlane(0, w, h, dst);
		copyPlane(1, w, h / 2, dst + size_t(w) * h);
		break;
	case VIDEO_FORMAT_I420:
		copyPlane(0, w, h, dst);
		copyPlane(1, w / 2, h / 2, dst + size_t(w) * h);
		copyPlane(2, w / 2, h / 2, dst + size_t(w) * h + size_t(w / 2) * (h / 2));
		break;
	case VIDEO_FORMAT_UYVY:
		copyPlane(0, w * 2, h, dst);
		break;
	default: /* BGRA, BGRX, RGBA */
		copyPlane(0, w * 4, h, dst);
		break;
	}
}

static void OutputRawVideo(void *data, video_data *frame)
{
	auto *o = static_cast<NdiOutput *>(data);
	if (!o->running.load() || !frame || !frame->data[0]) {
		return;
	}

	std::unique_ptr<VideoBuffer> buffer;
	{
		std::lock_guard<std::mutex> lock(o->poolMutex);
		if (!o->pool.empty()) {
			buffer = std::move(o->pool.back());
			o->pool.pop_back();
		}
	}
	if (!buffer) {
		/* all buffers queued or being sent: reuse the oldest queued frame (dropped) */
		if (auto oldest = o->queue->TryPop()) {
			buffer = std::move(*oldest);
			o->dropped++;
			droppedTotal++;
		}
	}
	if (!buffer) {
		/* the sender holds the only free one: drop this frame */
		o->dropped++;
		droppedTotal++;
		return;
	}
	if (buffer->data.size() != o->frameBytes) {
		ReturnBuffer(o, std::move(buffer));
		return;
	}

	PackFrame(o, frame, buffer->data.data());
	if (auto evicted = o->queue->Push(std::move(buffer))) {
		o->dropped++;
		droppedTotal++;
		ReturnBuffer(o, std::move(*evicted));
	}
}

static void OutputRawAudio(void *data, audio_data *frames)
{
	auto *o = static_cast<NdiOutput *>(data);
	if (!o->running.load() || !frames || frames->frames == 0) {
		return;
	}
	audio_t *audio = obs_output_audio(o->output);
	const audio_output_info *info = audio ? audio_output_get_info(audio) : nullptr;
	if (!info) {
		return;
	}
	const size_t channels = audio_output_get_channels(audio);
	if (channels == 0 || channels > MAX_AV_PLANES) {
		return;
	}
	const size_t samples = frames->frames;
	o->audioBuffer.resize(channels * samples);
	for (size_t ch = 0; ch < channels; ch++) {
		if (frames->data[ch]) {
			memcpy(&o->audioBuffer[ch * samples], frames->data[ch], samples * sizeof(float));
		} else {
			memset(&o->audioBuffer[ch * samples], 0, samples * sizeof(float));
		}
	}

	NDIlib_audio_frame_v3_t frame = {};
	frame.sample_rate = static_cast<int>(info->samples_per_sec);
	frame.no_channels = static_cast<int>(channels);
	frame.no_samples = static_cast<int>(samples);
	frame.timecode = NDIlib_send_timecode_synthesize;
	frame.FourCC = NDIlib_FourCC_audio_type_FLTP;
	frame.p_data = reinterpret_cast<uint8_t *>(o->audioBuffer.data());
	frame.channel_stride_in_bytes = static_cast<int>(samples * sizeof(float));
	o->lib->send_send_audio_v3(o->sender, &frame);
}

static bool OutputStart(void *data)
{
	auto *o = static_cast<NdiOutput *>(data);
	const NDIlib_v6 *lib = Lib();
	if (!lib || !SendingAllowed()) {
		obs_output_set_last_error(o->output, "NDI send is off");
		return false;
	}
	video_t *video = obs_output_video(o->output);
	audio_t *audio = obs_output_audio(o->output);
	const video_output_info *voi = video ? video_output_get_info(video) : nullptr;
	const audio_output_info *aoi = audio ? audio_output_get_info(audio) : nullptr;
	if (!voi || !aoi) {
		CTD_NDI_LOG(LOG_WARNING, "'%s': no video or audio to send", o->ndiName.c_str());
		return false;
	}

	o->width = voi->width;
	o->height = voi->height;
	o->fpsNum = static_cast<int>(voi->fps_num);
	o->fpsDen = static_cast<int>(voi->fps_den ? voi->fps_den : 1);
	switch (voi->format) {
	case VIDEO_FORMAT_NV12:
		o->format = VIDEO_FORMAT_NV12;
		o->fourcc = NDIlib_FourCC_video_type_NV12;
		break;
	case VIDEO_FORMAT_I420:
		o->format = VIDEO_FORMAT_I420;
		o->fourcc = NDIlib_FourCC_video_type_I420;
		break;
	case VIDEO_FORMAT_UYVY:
		o->format = VIDEO_FORMAT_UYVY;
		o->fourcc = NDIlib_FourCC_video_type_UYVY;
		break;
	case VIDEO_FORMAT_BGRA:
		o->format = VIDEO_FORMAT_BGRA;
		o->fourcc = NDIlib_FourCC_video_type_BGRA;
		break;
	case VIDEO_FORMAT_BGRX:
		o->format = VIDEO_FORMAT_BGRX;
		o->fourcc = NDIlib_FourCC_video_type_BGRX;
		break;
	case VIDEO_FORMAT_RGBA:
		o->format = VIDEO_FORMAT_RGBA;
		o->fourcc = NDIlib_FourCC_video_type_RGBA;
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
		o->format = VIDEO_FORMAT_NV12;
		o->fourcc = NDIlib_FourCC_video_type_NV12;
		break;
	}
	}
	if (o->width == 0 || o->height == 0 || (o->width % 2) || (o->height % 2)) {
		CTD_NDI_LOG(LOG_WARNING, "'%s': %ux%u can't be sent", o->ndiName.c_str(), o->width, o->height);
		return false;
	}
	switch (o->format) {
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_I420:
		o->stride = static_cast<int>(o->width);
		o->frameBytes = size_t(o->width) * o->height * 3 / 2;
		break;
	case VIDEO_FORMAT_UYVY:
		o->stride = static_cast<int>(o->width * 2);
		o->frameBytes = size_t(o->width) * o->height * 2;
		break;
	default:
		o->stride = static_cast<int>(o->width * 4);
		o->frameBytes = size_t(o->width) * o->height * 4;
		break;
	}

	audio_convert_info audioConversion = {};
	audioConversion.samples_per_sec = aoi->samples_per_sec;
	audioConversion.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audioConversion.speakers = aoi->speakers;
	obs_output_set_audio_conversion(o->output, &audioConversion);

	NDIlib_send_create_t config = {};
	config.p_ndi_name = o->ndiName.c_str();
	config.p_groups = nullptr;
	config.clock_video = false;
	config.clock_audio = false;
	o->lib = lib;
	o->sender = lib->send_create(&config);
	if (!o->sender) {
		CTD_NDI_LOG(LOG_ERROR, "'%s': NDI sender could not be created", o->ndiName.c_str());
		return false;
	}

	try {
		o->queue = std::make_unique<BoundedQueue<std::unique_ptr<VideoBuffer>>>(kSendQueue);
		{
			std::lock_guard<std::mutex> lock(o->poolMutex);
			o->pool.clear();
			for (size_t i = 0; i < kSendBuffers; i++) {
				auto buffer = std::make_unique<VideoBuffer>();
				buffer->data.resize(o->frameBytes);
				o->pool.push_back(std::move(buffer));
			}
		}
		o->dropped = 0;
		o->running = true;
		o->senderThread = std::thread(SenderMain, o);
	} catch (const std::exception &e) {
		CTD_NDI_LOG(LOG_ERROR, "'%s': could not start: %s", o->ndiName.c_str(), e.what());
		o->running = false;
		lib->send_destroy(o->sender);
		o->sender = nullptr;
		return false;
	}

	if (!obs_output_can_begin_data_capture(o->output, 0) || !obs_output_begin_data_capture(o->output, 0)) {
		CTD_NDI_LOG(LOG_ERROR, "'%s': OBS refused to start the data capture", o->ndiName.c_str());
		OutputStop(o, 0);
		return false;
	}

	activeSends++;
	o->counted = true;
	CTD_NDI_LOG(LOG_INFO, "Sending '%s': %ux%u %d/%d fps, %u Hz audio", o->ndiName.c_str(), o->width, o->height,
		    o->fpsNum, o->fpsDen, aoi->samples_per_sec);
	return true;
}

static void OutputStop(void *data, uint64_t)
{
	auto *o = static_cast<NdiOutput *>(data);
	obs_output_end_data_capture(o->output); /* no more raw_video / raw_audio after this */
	o->running = false;
	if (o->queue) {
		o->queue->Close();
	}
	if (o->senderThread.joinable()) {
		o->senderThread.join();
	}
	if (o->queue) {
		for (auto &buffer : o->queue->Drain()) {
			ReturnBuffer(o, std::move(buffer));
		}
	}
	if (o->sender && o->lib) {
		o->lib->send_destroy(o->sender);
	}
	o->sender = nullptr;
	{
		std::lock_guard<std::mutex> lock(o->poolMutex);
		o->pool.clear(); /* the frames' memory goes now, not at the next start */
	}
	if (o->counted) {
		o->counted = false;
		activeSends--;
		CTD_NDI_LOG(LOG_INFO, "Stopped sending '%s' (%llu frames dropped)", o->ndiName.c_str(),
			    static_cast<unsigned long long>(o->dropped.load()));
	}
}

void RegisterOutput()
{
	obs_output_info info = {};
	info.id = "ctd_ndi_output";
	info.flags = OBS_OUTPUT_AV;
	info.get_name = OutputName;
	info.create = OutputCreate;
	info.destroy = OutputDestroy;
	info.start = OutputStart;
	info.stop = OutputStop;
	info.raw_video = OutputRawVideo;
	info.raw_audio = OutputRawAudio;
	obs_register_output(&info);
}

} // namespace ctd_ndi
