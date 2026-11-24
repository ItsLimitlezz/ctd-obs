/******************************************************************************
    CTD OBS NDI module: one NDI sender (ndi-sender.hpp, docs/ctd/ndi.md).

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-sender.hpp"

#include <cstring>
#include <set>

namespace ctd_ndi {

constexpr size_t kSendQueue = 2;
constexpr size_t kSendBuffers = 3;

static std::mutex namesMutex;
static std::set<std::string> namesInUse;
static std::atomic<uint64_t> droppedTotal{0};

bool Sender::NameInUse(const std::string &name)
{
	std::lock_guard<std::mutex> lock(namesMutex);
	return namesInUse.count(CleanSenderName(name)) != 0;
}

uint64_t Sender::DroppedTotal()
{
	return droppedTotal.load();
}

std::shared_ptr<Sender> Sender::Create(const std::string &requested, std::string &error)
{
	const NDIlib_v6 *ndi = Lib();
	if (!ndi || !Running()) {
		error = "NDI is off";
		return nullptr;
	}
	const std::string clean = CleanSenderName(requested);
	if (clean.empty()) {
		error = "no sender name";
		return nullptr;
	}
	{
		std::lock_guard<std::mutex> lock(namesMutex);
		if (!namesInUse.insert(clean).second) {
			error = "another sender already uses the name '" + clean + "'";
			return nullptr;
		}
	}

	std::shared_ptr<Sender> sender(new Sender(clean));
	NDIlib_send_create_t config = {};
	config.p_ndi_name = sender->name.c_str();
	config.p_groups = nullptr;
	config.clock_video = false;
	config.clock_audio = false;
	sender->lib = ndi;
	sender->instance = ndi->send_create(&config);
	if (!sender->instance) {
		error = "NDI refused to create the sender '" + clean + "'";
		CTD_NDI_LOG(LOG_ERROR, "%s", error.c_str());
		return nullptr; /* the destructor gives the name back */
	}
	CTD_NDI_LOG(LOG_INFO, "NDI sender '%s' created", clean.c_str());
	return sender;
}

Sender::~Sender()
{
	StopVideo();
	{
		std::lock_guard<std::mutex> lock(audioMutex);
		if (instance && lib) {
			lib->send_destroy(instance);
			CTD_NDI_LOG(LOG_INFO, "NDI sender '%s' stopped (%llu frames dropped)", name.c_str(),
				    static_cast<unsigned long long>(dropped.load()));
		}
		instance = nullptr;
	}
	std::lock_guard<std::mutex> lock(namesMutex);
	namesInUse.erase(name);
}

void Sender::ReturnBuffer(std::unique_ptr<std::vector<uint8_t>> buffer)
{
	if (!buffer) {
		return;
	}
	std::lock_guard<std::mutex> lock(poolMutex);
	pool.push_back(std::move(buffer));
}

bool Sender::StartVideo(video_format format_, uint32_t width_, uint32_t height_, int fpsNum_, int fpsDen_,
			std::string &error)
{
	StopVideo();
	switch (format_) {
	case VIDEO_FORMAT_NV12:
		fourcc = NDIlib_FourCC_video_type_NV12;
		break;
	case VIDEO_FORMAT_I420:
		fourcc = NDIlib_FourCC_video_type_I420;
		break;
	case VIDEO_FORMAT_UYVY:
		fourcc = NDIlib_FourCC_video_type_UYVY;
		break;
	case VIDEO_FORMAT_BGRA:
		fourcc = NDIlib_FourCC_video_type_BGRA;
		break;
	case VIDEO_FORMAT_BGRX:
		fourcc = NDIlib_FourCC_video_type_BGRX;
		break;
	case VIDEO_FORMAT_RGBA:
		fourcc = NDIlib_FourCC_video_type_RGBA;
		break;
	default:
		error = "unsupported video format";
		return false;
	}
	if (width_ == 0 || height_ == 0 || (width_ % 2) || (height_ % 2)) {
		error = "unsupported size " + std::to_string(width_) + "x" + std::to_string(height_);
		return false;
	}
	format = format_;
	width = width_;
	height = height_;
	fpsNum = fpsNum_ > 0 ? fpsNum_ : 30;
	fpsDen = fpsDen_ > 0 ? fpsDen_ : 1;
	switch (format) {
	case VIDEO_FORMAT_NV12:
	case VIDEO_FORMAT_I420:
		stride = static_cast<int>(width);
		frameBytes = size_t(width) * height * 3 / 2;
		break;
	case VIDEO_FORMAT_UYVY:
		stride = static_cast<int>(width * 2);
		frameBytes = size_t(width) * height * 2;
		break;
	default:
		stride = static_cast<int>(width * 4);
		frameBytes = size_t(width) * height * 4;
		break;
	}

	try {
		queue = std::make_unique<BoundedQueue<std::unique_ptr<std::vector<uint8_t>>>>(kSendQueue);
		{
			std::lock_guard<std::mutex> lock(poolMutex);
			pool.clear();
			for (size_t i = 0; i < kSendBuffers; i++) {
				pool.push_back(std::make_unique<std::vector<uint8_t>>(frameBytes));
			}
		}
		videoRunning = true;
		thread = std::thread(&Sender::SenderMain, this);
	} catch (const std::exception &e) {
		videoRunning = false;
		error = std::string("could not start: ") + e.what();
		return false;
	}
	CTD_NDI_LOG(LOG_INFO, "NDI sender '%s': %ux%u %d/%d fps", name.c_str(), width, height, fpsNum, fpsDen);
	return true;
}

void Sender::StopVideo()
{
	videoRunning = false;
	if (queue) {
		queue->Close();
	}
	if (thread.joinable()) {
		thread.join();
	}
	if (queue) {
		queue->Drain();
	}
	queue.reset();
	std::lock_guard<std::mutex> lock(poolMutex);
	pool.clear(); /* the frames' memory goes now */
}

void Sender::SenderMain()
{
	while (videoRunning.load()) {
		auto buffer = queue->PopWait(std::chrono::milliseconds(100));
		if (!buffer || !*buffer) {
			continue;
		}
		try {
			NDIlib_video_frame_v2_t frame = {};
			frame.xres = static_cast<int>(width);
			frame.yres = static_cast<int>(height);
			frame.FourCC = fourcc;
			frame.frame_rate_N = fpsNum;
			frame.frame_rate_D = fpsDen;
			frame.picture_aspect_ratio = 0.0f; /* square pixels */
			frame.frame_format_type = NDIlib_frame_format_type_progressive;
			frame.timecode = NDIlib_send_timecode_synthesize;
			frame.p_data = (*buffer)->data();
			frame.line_stride_in_bytes = stride;
			/* synchronous: NDI is done with the buffer when this returns */
			lib->send_send_video_v2(instance, &frame);
		} catch (...) {
			CTD_NDI_LOG(LOG_WARNING, "'%s': NDI video send failed", name.c_str());
		}
		ReturnBuffer(std::move(*buffer));
	}
}

void Sender::PackFrame(const video_data *frame, uint8_t *dst) const
{
	auto copyPlane = [&](int plane, uint32_t rowBytes, uint32_t rows, uint8_t *out) {
		const uint8_t *src = frame->data[plane];
		const uint32_t srcStride = frame->linesize[plane];
		for (uint32_t y = 0; y < rows; y++) {
			memcpy(out + size_t(y) * rowBytes, src + size_t(y) * srcStride, rowBytes);
		}
	};
	const uint32_t w = width, h = height;
	switch (format) {
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

void Sender::PushVideo(const video_data *frame)
{
	if (!videoRunning.load() || !queue || !frame || !frame->data[0]) {
		return;
	}
	std::unique_ptr<std::vector<uint8_t>> buffer;
	{
		std::lock_guard<std::mutex> lock(poolMutex);
		if (!pool.empty()) {
			buffer = std::move(pool.back());
			pool.pop_back();
		}
	}
	if (!buffer) {
		/* all buffers queued or being sent: reuse the oldest queued frame (dropped) */
		if (auto oldest = queue->TryPop()) {
			buffer = std::move(*oldest);
			dropped++;
			droppedTotal++;
		}
	}
	if (!buffer) {
		/* the sender holds the only free one: drop this frame */
		dropped++;
		droppedTotal++;
		return;
	}
	if (buffer->size() != frameBytes) {
		ReturnBuffer(std::move(buffer));
		return;
	}
	PackFrame(frame, buffer->data());
	if (auto evicted = queue->Push(std::move(buffer))) {
		dropped++;
		droppedTotal++;
		ReturnBuffer(std::move(*evicted));
	}
}

void Sender::SendAudio(uint8_t *const *planes, size_t channels, uint32_t frames, uint32_t sampleRate)
{
	if (!planes || channels == 0 || channels > MAX_AV_PLANES || frames == 0 || sampleRate == 0) {
		return;
	}
	std::lock_guard<std::mutex> lock(audioMutex);
	if (!instance) {
		return;
	}
	const size_t samples = frames;
	audioBuffer.resize(channels * samples);
	for (size_t ch = 0; ch < channels; ch++) {
		if (planes[ch]) {
			memcpy(&audioBuffer[ch * samples], planes[ch], samples * sizeof(float));
		} else {
			memset(&audioBuffer[ch * samples], 0, samples * sizeof(float));
		}
	}
	NDIlib_audio_frame_v3_t frame = {};
	frame.sample_rate = static_cast<int>(sampleRate);
	frame.no_channels = static_cast<int>(channels);
	frame.no_samples = static_cast<int>(samples);
	frame.timecode = NDIlib_send_timecode_synthesize;
	frame.FourCC = NDIlib_FourCC_audio_type_FLTP;
	frame.p_data = reinterpret_cast<uint8_t *>(audioBuffer.data());
	frame.channel_stride_in_bytes = static_cast<int>(samples * sizeof(float));
	lib->send_send_audio_v3(instance, &frame);
}

} // namespace ctd_ndi
