/******************************************************************************
    CTD OBS NDI module: NDI® Source, receive (docs/ctd/ndi.md, "Receive").

      - one worker thread per source; it holds one of the 8 receiver slots
        while it has a receiver
      - NDIlib_recv_capture_v3 in a loop: each frame goes straight to libobs
        and is freed on the same iteration; NDI's queue is drained all the
        time, also while the source is hidden
      - unbuffered: libobs keeps only the newest frame, never a queue
      - off / paused / no slot / error / sender gone / unknown format: a
        placeholder frame and a status line, never a crash

    The type is registered even while NDI is off, so scenes keep their NDI
    sources and settings.

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-module.hpp"

#include <media-io/video-io.h>
#include <util/platform.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <exception>
#include <mutex>
#include <string>
#include <thread>

namespace ctd_ndi {

using Clock = std::chrono::steady_clock;

/* ------------------------------------------------------------------------------------------------ placeholder */

constexpr uint32_t kPlaceholderWidth = 640;
constexpr uint32_t kPlaceholderHeight = 360;

/* CTD Night: a dark slate with a thin magenta line at the bottom; BGRA */
static const std::vector<uint8_t> &PlaceholderPixels()
{
	static const std::vector<uint8_t> pixels = []() {
		std::vector<uint8_t> p(size_t(kPlaceholderWidth) * kPlaceholderHeight * 4);
		for (uint32_t y = 0; y < kPlaceholderHeight; y++) {
			const bool line = y >= kPlaceholderHeight - 6;
			for (uint32_t x = 0; x < kPlaceholderWidth; x++) {
				uint8_t *px = &p[(size_t(y) * kPlaceholderWidth + x) * 4];
				px[0] = line ? 0xF0 : 0x1A; /* B */
				px[1] = line ? 0x2A : 0x10; /* G */
				px[2] = line ? 0xE0 : 0x10; /* R */
				px[3] = 0xFF;
			}
		}
		return p;
	}();
	return pixels;
}

/* ------------------------------------------------------------------------------------------------ the source */

enum class State { Starting, Off, Paused, NoName, Limit, Connecting, Live, Lost, Error, Unsupported };

struct NdiSource {
	obs_source_t *source = nullptr;

	std::mutex settingsMutex;
	std::string ndiName;
	bool lowBandwidth = false;
	bool audio = true;
	std::atomic<uint64_t> settingsGeneration{0};

	std::mutex wakeMutex;
	std::condition_variable wake;
	std::atomic<bool> stop{false};
	std::thread worker;

	std::mutex statusMutex;
	State state = State::Starting;
	std::string status = "Starting";
};

static const char *SourceName(NdiSource *s)
{
	const char *name = obs_source_get_name(s->source);
	return name ? name : "";
}

static void OutputPlaceholder(NdiSource *s)
{
	const auto &pixels = PlaceholderPixels();
	obs_source_frame frame = {};
	frame.data[0] = const_cast<uint8_t *>(pixels.data());
	frame.linesize[0] = kPlaceholderWidth * 4;
	frame.width = kPlaceholderWidth;
	frame.height = kPlaceholderHeight;
	frame.format = VIDEO_FORMAT_BGRA;
	frame.full_range = true;
	frame.timestamp = os_gettime_ns();
	obs_source_output_video(s->source, &frame);
}

/* Logs once per change; anything but Connecting / Live shows the placeholder */
static void SetState(NdiSource *s, State state, const std::string &status)
{
	{
		std::lock_guard<std::mutex> lock(s->statusMutex);
		if (s->state == state && s->status == status) {
			return;
		}
		s->state = state;
		s->status = status;
	}
	CTD_NDI_LOG(state == State::Error || state == State::Unsupported ? LOG_WARNING : LOG_INFO, "'%s': %s",
		    SourceName(s), status.c_str());
	if (state != State::Connecting && state != State::Live) {
		OutputPlaceholder(s);
	}
}

static std::string Status(NdiSource *s)
{
	std::lock_guard<std::mutex> lock(s->statusMutex);
	return s->status;
}

/* Sleeps up to `ms`, waking early on settings changes, mode changes (the plugin wakes all sources) or destroy */
static void Wait(NdiSource *s, int ms, uint64_t settingsGen, uint64_t runGen)
{
	std::unique_lock<std::mutex> lock(s->wakeMutex);
	s->wake.wait_for(lock, std::chrono::milliseconds(ms), [&]() {
		return s->stop.load() || s->settingsGeneration.load() != settingsGen || RunGeneration() != runGen;
	});
}

/* ------------------------------------------------------------------------------------------------ frames to libobs */

/* false: a format the source doesn't handle (the frame is still freed by the caller) */
static bool OutputVideo(NdiSource *s, const NDIlib_video_frame_v2_t &v)
{
	if (!v.p_data || v.xres <= 0 || v.yres <= 0 || v.line_stride_in_bytes <= 0) {
		return false;
	}

	obs_source_frame frame = {};
	bool yuv = false;
	switch (v.FourCC) {
	case NDIlib_FourCC_video_type_UYVY:
	case NDIlib_FourCC_video_type_UYVA: /* the first plane is UYVY; the alpha plane after it isn't used */
		frame.format = VIDEO_FORMAT_UYVY;
		yuv = true;
		if (v.line_stride_in_bytes < v.xres * 2) {
			return false;
		}
		break;
	case NDIlib_FourCC_video_type_BGRA:
		frame.format = VIDEO_FORMAT_BGRA;
		break;
	case NDIlib_FourCC_video_type_BGRX:
		frame.format = VIDEO_FORMAT_BGRX;
		break;
	case NDIlib_FourCC_video_type_RGBA:
	case NDIlib_FourCC_video_type_RGBX:
		frame.format = VIDEO_FORMAT_RGBA;
		break;
	default:
		return false;
	}
	if (!yuv && v.line_stride_in_bytes < v.xres * 4) {
		return false;
	}

	frame.data[0] = v.p_data;
	frame.linesize[0] = static_cast<uint32_t>(v.line_stride_in_bytes);
	frame.width = static_cast<uint32_t>(v.xres);
	frame.height = static_cast<uint32_t>(v.yres);
	frame.timestamp = os_gettime_ns();
	if (yuv) {
		const video_colorspace cs = v.yres >= 720 ? VIDEO_CS_709 : VIDEO_CS_601;
		frame.full_range = false;
		video_format_get_parameters_for_format(cs, VIDEO_RANGE_PARTIAL, frame.format, frame.color_matrix,
						       frame.color_range_min, frame.color_range_max);
	} else {
		frame.full_range = true;
	}
	/* libobs copies the frame; unbuffered, so it keeps only the newest */
	obs_source_output_video(s->source, &frame);
	return true;
}

static speaker_layout Speakers(int channels)
{
	switch (channels) {
	case 1:
		return SPEAKERS_MONO;
	case 2:
		return SPEAKERS_STEREO;
	case 3:
		return SPEAKERS_2POINT1;
	case 4:
		return SPEAKERS_4POINT0;
	case 5:
		return SPEAKERS_4POINT1;
	case 6:
		return SPEAKERS_5POINT1;
	default:
		return channels >= 8 ? SPEAKERS_7POINT1 : SPEAKERS_STEREO;
	}
}

static void OutputAudio(NdiSource *s, const NDIlib_audio_frame_v3_t &a)
{
	if (!a.p_data || a.no_channels <= 0 || a.no_samples <= 0 || a.sample_rate <= 0 ||
	    a.FourCC != NDIlib_FourCC_audio_type_FLTP) {
		return;
	}
	int channels = a.no_channels;
	if (channels == 7) {
		channels = 6; /* no 7-channel layout in OBS */
	}
	if (channels > 8) {
		channels = 8;
	}
	if (a.no_channels > 1 && a.channel_stride_in_bytes < a.no_samples * int(sizeof(float))) {
		return;
	}

	obs_source_audio audio = {};
	for (int ch = 0; ch < channels && ch < MAX_AV_PLANES; ch++) {
		audio.data[ch] = a.p_data + size_t(ch) * size_t(a.channel_stride_in_bytes);
	}
	audio.frames = static_cast<uint32_t>(a.no_samples);
	audio.speakers = Speakers(channels);
	audio.format = AUDIO_FORMAT_FLOAT_PLANAR;
	audio.samples_per_sec = static_cast<uint32_t>(a.sample_rate);
	audio.timestamp = os_gettime_ns();
	obs_source_output_audio(s->source, &audio);
}

/* ------------------------------------------------------------------------------------------------ the worker */

static void Worker(NdiSource *s)
{
	NDIlib_recv_instance_t recv = nullptr;
	const NDIlib_v6 *recvLib = nullptr;
	SlotGuard slot;
	uint64_t seenSettings = ~0ull;
	uint64_t seenRun = ~0ull;
	Clock::time_point lastConnected = Clock::now();
	Clock::time_point lastCheck = Clock::now();
	Clock::time_point lastQueueWarning = Clock::now() - std::chrono::hours(1);

	auto drop = [&]() {
		if (recv && recvLib) {
			recvLib->recv_destroy(recv);
		}
		recv = nullptr;
		recvLib = nullptr;
		slot.Reset();
	};

	while (!s->stop.load()) {
		const uint64_t settingsGen = s->settingsGeneration.load();
		const uint64_t runGen = RunGeneration();
		try {
			const NDIlib_v6 *lib = Lib();
			if (!Running() || !lib) {
				drop();
				if (Paused()) {
					SetState(s, State::Paused, "NDI is paused (CTD ▸ NDI ▸ Resume NDI)");
				} else {
					SetState(s, State::Off, "NDI is off (Settings ▸ CTD ▸ NDI)");
				}
				Wait(s, 1000, settingsGen, runGen);
				continue;
			}

			if (settingsGen != seenSettings || runGen != seenRun) {
				drop();
				seenSettings = settingsGen;
				seenRun = runGen;
			}

			std::string name;
			bool low, audio;
			{
				std::lock_guard<std::mutex> lock(s->settingsMutex);
				name = s->ndiName;
				low = s->lowBandwidth;
				audio = s->audio;
			}
			if (name.empty()) {
				drop();
				SetState(s, State::NoName, "No NDI source chosen");
				Wait(s, 1000, settingsGen, runGen);
				continue;
			}

			if (!recv) {
				slot = SlotGuard(&Slots());
				if (!slot.Held()) {
					SetState(s, State::Limit,
						 "NDI receiver limit reached (" + std::to_string(Slots().Max()) +
							 " at once); waiting for a free one");
					Wait(s, 2000, settingsGen, runGen);
					continue;
				}
				NDIlib_recv_create_v3_t config = {};
				config.source_to_connect_to.p_ndi_name = name.c_str();
				config.source_to_connect_to.p_url_address = nullptr;
				config.color_format = NDIlib_recv_color_format_UYVY_BGRA;
				config.bandwidth = low ? NDIlib_recv_bandwidth_lowest : NDIlib_recv_bandwidth_highest;
				config.allow_video_fields = false;
				config.p_ndi_recv_name = "CTD OBS";
				recv = lib->recv_create_v3(&config);
				if (!recv) {
					slot.Reset();
					SetState(s, State::Error,
						 "Could not create an NDI receiver for '" + name + "'");
					Wait(s, 2000, settingsGen, runGen);
					continue;
				}
				recvLib = lib;
				lastConnected = Clock::now();
				SetState(s, State::Connecting, "Connecting to '" + name + "'");
			}

			NDIlib_video_frame_v2_t video = {};
			NDIlib_audio_frame_v3_t audioFrame = {};
			NDIlib_metadata_frame_t metadata = {};
			const NDIlib_frame_type_e type =
				lib->recv_capture_v3(recv, &video, audio ? &audioFrame : nullptr, &metadata, 100);
			switch (type) {
			case NDIlib_frame_type_video: {
				const bool ok = OutputVideo(s, video);
				lib->recv_free_video_v2(recv, &video);
				if (ok) {
					lastConnected = Clock::now();
					SetState(s, State::Live, "Receiving '" + name + "'");
				} else {
					SetState(s, State::Unsupported,
						 "'" + name + "' sends a video format CTD OBS doesn't handle");
				}
				break;
			}
			case NDIlib_frame_type_audio:
				OutputAudio(s, audioFrame);
				lib->recv_free_audio_v3(recv, &audioFrame);
				break;
			case NDIlib_frame_type_metadata:
				lib->recv_free_metadata(recv, &metadata);
				break;
			case NDIlib_frame_type_error:
				drop();
				SetState(s, State::Error,
					 "NDI reported an error receiving '" + name + "'; reconnecting");
				Wait(s, 2000, settingsGen, runGen);
				continue;
			default:
				break;
			}

			const auto now = Clock::now();
			if (recv && now - lastCheck >= std::chrono::seconds(1)) {
				lastCheck = now;
				if (lib->recv_get_no_connections(recv) > 0) {
					lastConnected = now;
				} else if (now - lastConnected >= std::chrono::seconds(3)) {
					/* NDI reconnects by itself when the sender comes back */
					SetState(s, State::Lost, "'" + name + "' is not reachable");
				}
				NDIlib_recv_queue_t queue = {};
				lib->recv_get_queue(recv, &queue);
				if (queue.video_frames > 4 && now - lastQueueWarning >= std::chrono::seconds(60)) {
					lastQueueWarning = now;
					CTD_NDI_LOG(LOG_WARNING, "'%s': NDI receive queue holds %d video frames",
						    SourceName(s), queue.video_frames);
				}
			}
		} catch (const std::exception &e) {
			drop();
			SetState(s, State::Error, std::string("Receive error: ") + e.what());
			Wait(s, 2000, settingsGen, runGen);
		} catch (...) {
			drop();
			SetState(s, State::Error, "Receive error");
			Wait(s, 2000, settingsGen, runGen);
		}
	}
	drop();
}

/* ------------------------------------------------------------------------------------------------ obs_source_info */

static const char *GetName(void *)
{
	return obs_module_text("NDISource");
}

static void Update(void *data, obs_data_t *settings)
{
	auto *s = static_cast<NdiSource *>(data);
	{
		std::lock_guard<std::mutex> lock(s->settingsMutex);
		const char *name = obs_data_get_string(settings, "ndi_name");
		s->ndiName = name ? name : "";
		const char *bandwidth = obs_data_get_string(settings, "bandwidth");
		s->lowBandwidth = bandwidth && strcmp(bandwidth, "lowest") == 0;
		s->audio = obs_data_get_bool(settings, "audio");
	}
	s->settingsGeneration++;
	s->wake.notify_all();
}

static void *Create(obs_data_t *settings, obs_source_t *source)
{
	auto *s = new NdiSource();
	s->source = source;
	obs_source_set_async_unbuffered(source, true);
	Update(s, settings);
	OutputPlaceholder(s);
	try {
		s->worker = std::thread(Worker, s);
	} catch (const std::exception &e) {
		CTD_NDI_LOG(LOG_ERROR, "'%s': worker thread failed: %s", SourceName(s), e.what());
	}
	return s;
}

static void Destroy(void *data)
{
	auto *s = static_cast<NdiSource *>(data);
	s->stop = true;
	s->wake.notify_all();
	if (s->worker.joinable()) {
		s->worker.join();
	}
	delete s;
}

static void Defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "ndi_name", "");
	obs_data_set_default_string(settings, "bandwidth", "highest");
	obs_data_set_default_bool(settings, "audio", true);
}

static bool Refresh(obs_properties_t *, obs_property_t *, void *)
{
	return true; /* rebuild the properties: the list is the finder's current one */
}

static obs_properties_t *Properties(void *data)
{
	auto *s = static_cast<NdiSource *>(data);
	obs_properties_t *props = obs_properties_create();

	if (!Running()) {
		obs_properties_add_text(props, "ndi_off", obs_module_text("NDISource.Off"), OBS_TEXT_INFO);
	} else {
		FinderEnsureStarted();
	}

	obs_property_t *list = obs_properties_add_list(props, "ndi_name", obs_module_text("NDISource.Name"),
						       OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	std::string current;
	if (s) {
		std::lock_guard<std::mutex> lock(s->settingsMutex);
		current = s->ndiName;
	}
	bool listed = false;
	for (const std::string &name : FinderSources()) {
		obs_property_list_add_string(list, name.c_str(), name.c_str());
		listed = listed || name == current;
	}
	if (!current.empty() && !listed) {
		obs_property_list_add_string(list, current.c_str(), current.c_str());
	}
	obs_properties_add_button2(props, "refresh", obs_module_text("NDISource.Refresh"), Refresh, nullptr);

	obs_property_t *bandwidth = obs_properties_add_list(props, "bandwidth", obs_module_text("NDISource.Bandwidth"),
							    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(bandwidth, obs_module_text("NDISource.Bandwidth.Highest"), "highest");
	obs_property_list_add_string(bandwidth, obs_module_text("NDISource.Bandwidth.Lowest"), "lowest");

	obs_properties_add_bool(props, "audio", obs_module_text("NDISource.Audio"));

	if (s) {
		const std::string status = std::string(obs_module_text("NDISource.Status")) + " " + Status(s);
		obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);
	}
	obs_properties_add_text(props, "ndi_about", obs_module_text("NDI.Attribution"), OBS_TEXT_INFO);
	return props;
}

void RegisterSource()
{
	obs_source_info info = {};
	info.id = "ctd_ndi_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_ASYNC_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE;
	info.icon_type = OBS_ICON_TYPE_MEDIA;
	info.get_name = GetName;
	info.create = Create;
	info.destroy = Destroy;
	info.update = Update;
	info.get_defaults = Defaults;
	info.get_properties = Properties;
	obs_register_source(&info);
}

} // namespace ctd_ndi
