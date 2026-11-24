/******************************************************************************
    CTD OBS NDI module: per-source senders, the "NDI® Output" filter
    (ctd_ndi_filter; docs/ctd/ndi.md, "Per-source senders").

    Add it to any source, scene or group; it publishes that source as its
    own NDI sender (the name in its settings, or the source's name), with
    the source's own audio if it has any. It is saved with the scene
    collection like any filter. Disabling the filter stops the sender.

    It runs only while all of these hold (DecideSender, ndi-logic.cpp):
    NDI is on and not paused, the filter is enabled, its source is active
    (on air), the source has a picture (or is audio only), the name is free,
    and one of the 8 sender slots is free. Otherwise nothing of it exists:
    no canvas, no output, no sender, nothing rendered.

    While it runs:
      video   a private canvas (EPHEMERAL, no ACTIVATE: it never makes the
              source active itself) the size of the source, with the source
              on channel 0, and a ctd_ndi_source_output on that canvas's
              video: OBS's own render, GPU conversion and readback, into the
              sender's bounded queue (ndi-sender.cpp)
      audio   filter_audio sends the source's audio as it passes through

    video_tick (graphics thread) only notices that something may have
    changed and queues the decision on the UI thread, where canvases and
    outputs are created and destroyed.

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-sender.hpp"

#include <media-io/audio-io.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <set>

namespace ctd_ndi {

/* What a running sender holds; detached as a whole when it stops, so it can be freed on the UI thread */
struct SenderResources {
	obs_canvas_t *canvas = nullptr;
	obs_output_t *output = nullptr;
	std::shared_ptr<Sender> sender;
	SlotGuard slot;
	std::string key;
};

struct NdiFilter {
	obs_source_t *filter = nullptr;
	std::string key; /* the filter's UUID: the sender registry key */

	std::mutex settingsMutex;
	std::string requestedName;
	std::atomic<bool> audio{true};
	std::atomic<uint64_t> settingsGeneration{0};

	/* video_tick → UI: the signature of the inputs last decided on, and whether a decision is queued */
	std::atomic<uint64_t> decidedSignature{0};
	std::atomic<bool> taskQueued{false};

	/* UI thread only */
	SenderResources running;
	bool isRunning = false;
	std::string runningName;
	uint32_t canvasWidth = 0, canvasHeight = 0;
	bool runningAudioOnly = false;

	/* filter_audio (audio thread) reads this */
	std::mutex audioMutex;
	std::shared_ptr<Sender> audioSender;

	std::mutex stateMutex;
	std::string state = "NDI is off";
};

static std::mutex filtersMutex;
static std::set<NdiFilter *> filters;

/* ------------------------------------------------------------------------------------------------ helpers */

static uint64_t Mix(uint64_t h, uint64_t v)
{
	return (h ^ (v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2)));
}

/* Everything the decision depends on that can change without the filter being told */
static uint64_t Signature(NdiFilter *f)
{
	obs_source_t *parent = obs_filter_get_parent(f->filter);
	uint64_t h = 1469598103934665603ull;
	h = Mix(h, Lib() && ModeSends(CurrentMode()) && !Switching());
	h = Mix(h, Paused());
	h = Mix(h, RunGeneration());
	h = Mix(h, f->settingsGeneration.load());
	h = Mix(h, obs_source_enabled(f->filter));
	h = Mix(h, reinterpret_cast<uintptr_t>(parent));
	if (parent) {
		h = Mix(h, obs_source_active(parent));
		h = Mix(h, obs_source_get_width(parent));
		h = Mix(h, obs_source_get_height(parent));
		h = Mix(h, (obs_source_get_output_flags(parent) & OBS_SOURCE_VIDEO) != 0);
	}
	return h | 1; /* never 0, the "nothing decided yet" value */
}

static void SetState(NdiFilter *f, const std::string &state)
{
	std::string previous;
	{
		std::lock_guard<std::mutex> lock(f->stateMutex);
		if (f->state == state) {
			return;
		}
		previous = f->state;
		f->state = state;
	}
	const char *filterName = obs_source_get_name(f->filter);
	obs_source_t *parent = obs_filter_get_parent(f->filter);
	const char *parentName = parent ? obs_source_get_name(parent) : nullptr;
	CTD_NDI_LOG(LOG_INFO, "'%s' on '%s': %s", filterName ? filterName : "", parentName ? parentName : "(none)",
		    state.c_str());
}

static void FreeResources(SenderResources &r)
{
	if (r.output) {
		if (obs_output_active(r.output)) {
			obs_output_force_stop(r.output);
		}
		obs_output_release(r.output);
		r.output = nullptr;
	}
	if (!r.key.empty()) {
		SenderRegistryRemove(r.key);
	}
	if (r.canvas) {
		obs_canvas_set_channel(r.canvas, 0, nullptr);
		obs_canvas_remove(r.canvas);
		obs_canvas_release(r.canvas);
		r.canvas = nullptr;
	}
	r.sender.reset(); /* the NDI sender goes here (its output has released it) */
	r.slot.Reset();
}

static void QueueReconcileAll();

/* Stops this filter's sender. UI thread. */
static void Stop(NdiFilter *f)
{
	{
		std::lock_guard<std::mutex> lock(f->audioMutex);
		f->audioSender.reset();
	}
	const bool was = f->isRunning;
	FreeResources(f->running);
	f->running = SenderResources();
	f->isRunning = false;
	f->runningName.clear();
	f->canvasWidth = f->canvasHeight = 0;
	if (was) {
		/* a slot and a name are free again: senders waiting for them may start */
		QueueReconcileAll();
	}
}

static bool Start(NdiFilter *f, obs_source_t *parent, const std::string &name, bool audioOnly, uint32_t sourceWidth,
		  uint32_t sourceHeight, uint32_t outWidth, uint32_t outHeight, std::string &error)
{
	SenderResources r;
	r.slot = SlotGuard(&Slots());
	if (!r.slot.Held()) {
		error = SenderStateText(SenderState::Limit);
		return false;
	}
	r.sender = Sender::Create(name, error);
	if (!r.sender) {
		FreeResources(r);
		return false;
	}

	if (!audioOnly) {
		obs_video_info ovi = {};
		if (!obs_get_video_info(&ovi)) {
			error = "no video";
			FreeResources(r);
			return false;
		}
		/* the source renders at its own size into the canvas; OBS scales to the output size if it had to shrink */
		ovi.base_width = std::max<uint32_t>(2, sourceWidth & ~1u);
		ovi.base_height = std::max<uint32_t>(2, sourceHeight & ~1u);
		ovi.output_width = outWidth;
		ovi.output_height = outHeight;
		const std::string canvasName = "CTD NDI: " + name;
		r.canvas = obs_canvas_create_private(canvasName.c_str(), &ovi, EPHEMERAL);
		if (!r.canvas || !obs_canvas_get_video(r.canvas)) {
			error = "the sender's canvas could not be created";
			FreeResources(r);
			return false;
		}
		obs_canvas_set_channel(r.canvas, 0, parent);

		r.key = f->key;
		SenderRegistryPut(r.key, r.sender);
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "sender_key", r.key.c_str());
		const std::string outputName = "CTD NDI send: " + name;
		r.output = obs_output_create("ctd_ndi_source_output", outputName.c_str(), settings, nullptr);
		obs_data_release(settings);
		if (!r.output) {
			error = "the sender's output could not be created";
			FreeResources(r);
			return false;
		}
		obs_output_set_media(r.output, obs_canvas_get_video(r.canvas), obs_get_audio());
		if (!obs_output_start(r.output)) {
			const char *why = obs_output_get_last_error(r.output);
			error = std::string("the sender's output did not start") + (why ? std::string(": ") + why : "");
			FreeResources(r);
			return false;
		}
	}

	{
		std::lock_guard<std::mutex> lock(f->audioMutex);
		f->audioSender = r.sender;
	}
	f->running = std::move(r);
	f->isRunning = true;
	f->runningName = name;
	f->canvasWidth = outWidth;
	f->canvasHeight = outHeight;
	f->runningAudioOnly = audioOnly;
	return true;
}

/* Decides and acts. UI thread. */
static void Reconcile(NdiFilter *f)
{
	try {
		/* the inputs are read here, so a change during this call re-queues it */
		f->decidedSignature = Signature(f);

		obs_source_t *parent = obs_filter_get_parent(f->filter);
		std::string name;
		{
			std::lock_guard<std::mutex> lock(f->settingsMutex);
			name = f->requestedName;
		}
		if (CleanSenderName(name).empty() && parent) {
			const char *parentName = obs_source_get_name(parent);
			name = parentName ? parentName : "";
		}
		name = CleanSenderName(name);

		SenderInputs in;
		in.ndiOn = Lib() && ModeSends(CurrentMode()) && !Switching();
		in.paused = Paused();
		in.filterEnabled = obs_source_enabled(f->filter);
		in.sourceActive = parent && obs_source_active(parent);
		in.audioOnly = parent && !(obs_source_get_output_flags(parent) & OBS_SOURCE_VIDEO);
		in.width = parent ? obs_source_get_width(parent) : 0;
		in.height = parent ? obs_source_get_height(parent) : 0;
		in.nameUsable = !name.empty();
		in.nameTaken = name != f->runningName && Sender::NameInUse(name);
		in.slotFree = f->isRunning || Slots().InUse() < Slots().Max();

		const SenderState decision = DecideSender(in);
		if (decision != SenderState::Run) {
			Stop(f);
			SetState(f, SenderStateText(decision));
			return;
		}

		uint32_t outWidth = 0, outHeight = 0;
		SenderCanvasSize(in.width, in.height, outWidth, outHeight);
		const bool same = f->isRunning && name == f->runningName && in.audioOnly == f->runningAudioOnly &&
				  (in.audioOnly || (outWidth == f->canvasWidth && outHeight == f->canvasHeight));
		if (same) {
			SetState(f, in.audioOnly ? "Sending (audio only)" : SenderStateText(SenderState::Run));
			return;
		}

		Stop(f);
		std::string error;
		if (Start(f, parent, name, in.audioOnly, in.width, in.height, outWidth, outHeight, error)) {
			SetState(f, in.audioOnly ? "Sending (audio only)" : SenderStateText(SenderState::Run));
		} else {
			SetState(f, "Not sending: " + error);
		}
	} catch (const std::exception &e) {
		Stop(f);
		SetState(f, std::string("Not sending: ") + e.what());
	} catch (...) {
		Stop(f);
		SetState(f, "Not sending: error");
	}
}

/* ------------------------------------------------------------------------------------------------ UI tasks */

struct ReconcileTask {
	obs_weak_source_t *weak;
};

static void RunReconcileTask(void *param)
{
	auto *task = static_cast<ReconcileTask *>(param);
	obs_source_t *source = obs_weak_source_get_source(task->weak);
	obs_weak_source_release(task->weak);
	delete task;
	if (!source) {
		return;
	}
	auto *f = static_cast<NdiFilter *>(obs_obj_get_data(source));
	if (f) {
		f->taskQueued = false;
		Reconcile(f);
	}
	obs_source_release(source);
}

static void QueueReconcile(NdiFilter *f)
{
	if (f->taskQueued.exchange(true)) {
		return;
	}
	auto *task = new ReconcileTask{obs_source_get_weak_source(f->filter)};
	obs_queue_task(OBS_TASK_UI, RunReconcileTask, task, false);
}

static std::vector<obs_weak_source_t *> AllFilters()
{
	std::vector<obs_weak_source_t *> out;
	std::lock_guard<std::mutex> lock(filtersMutex);
	for (NdiFilter *f : filters) {
		out.push_back(obs_source_get_weak_source(f->filter));
	}
	return out;
}

static void ForEachFilter(const std::function<void(NdiFilter *)> &fn)
{
	for (obs_weak_source_t *weak : AllFilters()) {
		obs_source_t *source = obs_weak_source_get_source(weak);
		obs_weak_source_release(weak);
		if (!source) {
			continue;
		}
		if (auto *f = static_cast<NdiFilter *>(obs_obj_get_data(source))) {
			fn(f);
		}
		obs_source_release(source);
	}
}

static std::atomic<bool> reconcileAllQueued{false};

static void RunReconcileAll(void *)
{
	reconcileAllQueued = false;
	FiltersReconcileAll();
}

static void QueueReconcileAll()
{
	if (!reconcileAllQueued.exchange(true)) {
		obs_queue_task(OBS_TASK_UI, RunReconcileAll, nullptr, false);
	}
}

void FiltersReconcileAll()
{
	ForEachFilter([](NdiFilter *f) { Reconcile(f); });
}

void FiltersStopAll()
{
	ForEachFilter([](NdiFilter *f) {
		Stop(f);
		SetState(f, SenderStateText(SenderState::Off));
		f->decidedSignature = 0;
	});
}

std::string FilterState(obs_source_t *filter)
{
	const char *id = filter ? obs_source_get_id(filter) : nullptr;
	if (!id || strcmp(id, "ctd_ndi_filter") != 0) {
		return std::string();
	}
	auto *f = static_cast<NdiFilter *>(obs_obj_get_data(filter));
	if (!f) {
		return std::string();
	}
	std::lock_guard<std::mutex> lock(f->stateMutex);
	return f->state;
}

/* ------------------------------------------------------------------------------------------------ obs_source_info */

static const char *GetName(void *)
{
	return obs_module_text("NDIFilter");
}

static void Update(void *data, obs_data_t *settings)
{
	auto *f = static_cast<NdiFilter *>(data);
	{
		std::lock_guard<std::mutex> lock(f->settingsMutex);
		const char *name = obs_data_get_string(settings, "ndi_name");
		f->requestedName = name ? name : "";
	}
	f->audio = obs_data_get_bool(settings, "audio");
	f->settingsGeneration++;
}

static void *Create(obs_data_t *settings, obs_source_t *source)
{
	auto *f = new NdiFilter();
	f->filter = source;
	const char *uuid = obs_source_get_uuid(source);
	f->key = uuid ? uuid : "";
	Update(f, settings);
	std::lock_guard<std::mutex> lock(filtersMutex);
	filters.insert(f);
	return f;
}

struct CleanupTask {
	SenderResources resources;
};

static void RunCleanupTask(void *param)
{
	auto *task = static_cast<CleanupTask *>(param);
	FreeResources(task->resources);
	delete task;
	QueueReconcileAll();
}

static void Destroy(void *data)
{
	auto *f = static_cast<NdiFilter *>(data);
	{
		std::lock_guard<std::mutex> lock(filtersMutex);
		filters.erase(f);
	}
	{
		std::lock_guard<std::mutex> lock(f->audioMutex);
		f->audioSender.reset();
	}
	if (f->isRunning) {
		if (obs_in_task_thread(OBS_TASK_UI)) {
			Stop(f);
		} else {
			/* canvases and outputs go on the UI thread (a source can be freed on the graphics thread) */
			auto *task = new CleanupTask{std::move(f->running)};
			obs_queue_task(OBS_TASK_UI, RunCleanupTask, task, false);
		}
	}
	delete f;
}

static void Defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "ndi_name", "");
	obs_data_set_default_bool(settings, "audio", true);
}

static obs_properties_t *Properties(void *data)
{
	auto *f = static_cast<NdiFilter *>(data);
	obs_properties_t *props = obs_properties_create();
	if (!Running()) {
		obs_properties_add_text(props, "ndi_off", obs_module_text("NDIFilter.Off"), OBS_TEXT_INFO);
	}
	obs_property_t *name =
		obs_properties_add_text(props, "ndi_name", obs_module_text("NDIFilter.Name"), OBS_TEXT_DEFAULT);
	obs_property_set_long_description(name, obs_module_text("NDIFilter.Name.Help"));
	obs_properties_add_bool(props, "audio", obs_module_text("NDIFilter.Audio"));
	if (f) {
		std::string state;
		{
			std::lock_guard<std::mutex> lock(f->stateMutex);
			state = f->state;
		}
		const std::string status = std::string(obs_module_text("NDIFilter.Status")) + " " + state;
		obs_properties_add_text(props, "status", status.c_str(), OBS_TEXT_INFO);
	}
	obs_properties_add_text(props, "ndi_about", obs_module_text("NDI.Attribution"), OBS_TEXT_INFO);
	return props;
}

static void VideoTick(void *data, float)
{
	auto *f = static_cast<NdiFilter *>(data);
	if (Signature(f) != f->decidedSignature.load()) {
		QueueReconcile(f);
	}
}

static void VideoRender(void *data, gs_effect_t *)
{
	auto *f = static_cast<NdiFilter *>(data);
	obs_source_skip_video_filter(f->filter);
}

static obs_audio_data *FilterAudio(void *data, obs_audio_data *audio)
{
	auto *f = static_cast<NdiFilter *>(data);
	if (!audio || !f->audio.load()) {
		return audio;
	}
	std::shared_ptr<Sender> sender;
	{
		std::lock_guard<std::mutex> lock(f->audioMutex);
		sender = f->audioSender;
	}
	if (sender) {
		audio_t *mix = obs_get_audio();
		const audio_output_info *info = mix ? audio_output_get_info(mix) : nullptr;
		if (info) {
			sender->SendAudio(audio->data, audio_output_get_channels(mix), audio->frames,
					  info->samples_per_sec);
		}
	}
	return audio; /* unchanged */
}

void RegisterFilter()
{
	obs_source_info info = {};
	info.id = "ctd_ndi_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO;
	info.get_name = GetName;
	info.create = Create;
	info.destroy = Destroy;
	info.update = Update;
	info.get_defaults = Defaults;
	info.get_properties = Properties;
	info.video_tick = VideoTick;
	info.video_render = VideoRender;
	info.filter_audio = FilterAudio;
	obs_register_source(&info);
}

} // namespace ctd_ndi
