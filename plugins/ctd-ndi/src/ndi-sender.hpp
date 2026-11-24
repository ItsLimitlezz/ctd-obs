/******************************************************************************
    CTD OBS NDI module: one NDI sender (docs/ctd/ndi.md, "Send").

      video   PushVideo (OBS's video thread) copies the frame into one of 3
              preallocated buffers and pushes it into a BoundedQueue of 2
              (full: the oldest is dropped and its buffer reused); a sender
              thread sends with NDIlib_send_send_video_v2, synchronous, so
              NDI never reads a buffer that is reused. Memory is fixed at 3
              frames whatever NDI or the network does.
      audio   SendAudio (OBS's audio thread): planar float copied into one
              reusable buffer, NDIlib_send_send_audio_v3

    Names are unique among the senders running in CTD OBS: Create refuses a
    name that is in use.

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ndi-module.hpp"

#include <media-io/video-io.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <thread>

namespace ctd_ndi {

class Sender {
public:
	/* nullptr with an error: NDI isn't running, the name is unusable or taken, or NDI refused */
	static std::shared_ptr<Sender> Create(const std::string &name, std::string &error);
	/* Whether a running sender uses this name (cleaned) */
	static bool NameInUse(const std::string &name);
	/* Frames dropped by every sender since CTD OBS started */
	static uint64_t DroppedTotal();

	~Sender();
	Sender(const Sender &) = delete;
	Sender &operator=(const Sender &) = delete;

	/* Video layout for PushVideo; starts the sender thread. format must be NV12, I420, UYVY, BGRA, BGRX or RGBA. */
	bool StartVideo(video_format format, uint32_t width, uint32_t height, int fpsNum, int fpsDen,
			std::string &error);
	void StopVideo();
	void PushVideo(const video_data *frame);

	void SendAudio(uint8_t *const *planes, size_t channels, uint32_t frames, uint32_t sampleRate);

	const std::string &Name() const { return name; }
	uint64_t Dropped() const { return dropped.load(); }

private:
	explicit Sender(std::string name_) : name(std::move(name_)) {}
	void SenderMain();
	void ReturnBuffer(std::unique_ptr<std::vector<uint8_t>> buffer);
	void PackFrame(const video_data *frame, uint8_t *dst) const;

	const std::string name;
	const NDIlib_v6 *lib = nullptr;
	NDIlib_send_instance_t instance = nullptr;

	/* video layout, fixed between StartVideo and StopVideo */
	NDIlib_FourCC_video_type_e fourcc = NDIlib_FourCC_video_type_NV12;
	video_format format = VIDEO_FORMAT_NV12;
	uint32_t width = 0, height = 0;
	int stride = 0;
	size_t frameBytes = 0;
	int fpsNum = 30, fpsDen = 1;

	std::unique_ptr<BoundedQueue<std::unique_ptr<std::vector<uint8_t>>>> queue;
	std::mutex poolMutex;
	std::vector<std::unique_ptr<std::vector<uint8_t>>> pool;
	std::thread thread;
	std::atomic<bool> videoRunning{false};
	std::atomic<uint64_t> dropped{0};

	std::mutex audioMutex;
	std::vector<float> audioBuffer;
};

/* Hands a per-source filter's sender to its video output (ctd_ndi_source_output, settings "sender_key"), keyed by the
 * filter's UUID */
void SenderRegistryPut(const std::string &key, std::shared_ptr<Sender> sender);
void SenderRegistryRemove(const std::string &key);
std::shared_ptr<Sender> SenderRegistryGet(const std::string &key);

} // namespace ctd_ndi
