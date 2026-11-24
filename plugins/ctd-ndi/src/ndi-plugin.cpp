/******************************************************************************
    CTD OBS NDI module (ctd-ndi): NDI® send from inside CTD OBS's own
    process, with no system pieces (docs/ctd/ndi.md). CTD OBS only sends
    NDI; it doesn't receive it.

      program    "CTD OBS Program": the main video and audio
      vertical   "CTD OBS Vertical": the vertical canvas, main audio
      per source the "NDI® Output" filter on any source, scene or group
                 (ndi-filter.cpp), saved with the scene collection

    At most 8 senders run at once, all kinds together.

    Mode ([CTD] NDIMode in user.ini): off (default) / send. While off, the
    NDI library isn't loaded. --ctd-no-ndi and OBS's safe mode force off for
    the launch. Switching to off stops every sender at once.

    Global procs (obs_get_proc_handler()), for the CTD OBS app:

      void ctd_ndi_apply()
          Reads [CTD] NDIMode / NDISendProgram / NDISendVertical and applies
          them now (Settings ▸ CTD ▸ NDI, OK/Apply).
      void ctd_ndi_setup(out bool ok, out string message)
          Set up from the installed NDI 6 Runtime (allow-listed SHA-256
          only), then applies the mode.
      void ctd_ndi_pause(in bool paused)
          Pauses / resumes every sender without changing the setting
          (CTD ▸ NDI, the memory watchdog).
      void ctd_ndi_status(out string mode, out string configured,
                          out bool paused, out bool launch_disabled,
                          out bool loaded, out string version,
                          out string installed, out string message,
                          out int senders, out int max_senders,
                          out int dropped, out bool program,
                          out bool vertical, out string system_pieces)
      void ctd_ndi_filter_state(in ptr filter, out string state)
          The state line of one NDI Output filter (Settings' outputs list).

    Called here, registered by the CTD OBS app before modules load:

      void ctd_ndi_launch_disabled(out bool disabled)
          true for a --ctd-no-ndi launch

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-sender.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <atomic>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("ctd-ndi", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "NDI send for CTD OBS";
}

namespace ctd_ndi {

static const char *const kSection = "CTD";
static const char *const kKeyMode = "NDIMode";
static const char *const kKeySendProgram = "NDISendProgram";
static const char *const kKeySendVertical = "NDISendVertical";

static const char *const kProgramName = "CTD OBS Program";
static const char *const kVerticalName = "CTD OBS Vertical";
static const char *const kVerticalCanvas = "Vertical";

static Mode configuredMode = Mode::Off;
static std::atomic<Mode> currentMode{Mode::Off};
static std::atomic<bool> paused{false};
/* true until loading finishes, during profile / collection switches and from exit on: no sender starts */
static std::atomic<bool> switching{true};
static std::atomic<uint64_t> runGeneration{0};
static bool launchDisabled = false;
static bool sendProgram = true;
static bool sendVertical = false;
static std::string lastMessage;

/* UI thread only */
static bool loadingFinished = false;

struct CanvasSend {
	obs_output_t *output = nullptr;
	SlotGuard slot;
};
static CanvasSend programSend;
static CanvasSend verticalSend;

static SenderSlots slots(kMaxSenders);

Mode CurrentMode()
{
	return currentMode.load();
}

bool Paused()
{
	return paused.load();
}

bool Running()
{
	return Lib() != nullptr && ModeSends(currentMode.load()) && !paused.load();
}

bool Switching()
{
	return switching.load();
}

uint64_t RunGeneration()
{
	return runGeneration.load();
}

SenderSlots &Slots()
{
	return slots;
}

/* ------------------------------------------------------------------------------------------------ config */

static void ReadConfig()
{
	config_t *config = obs_frontend_get_user_config();
	if (!config) {
		configuredMode = Mode::Off;
		return;
	}
	config_set_default_string(config, kSection, kKeyMode, "off");
	config_set_default_bool(config, kSection, kKeySendProgram, true);
	config_set_default_bool(config, kSection, kKeySendVertical, false);
	configuredMode = ParseMode(config_get_string(config, kSection, kKeyMode));
	sendProgram = config_get_bool(config, kSection, kKeySendProgram);
	sendVertical = config_get_bool(config, kSection, kKeySendVertical);
}

/* ------------------------------------------------------------------------------------------------ program / vertical */

static void StopSend(CanvasSend &send)
{
	if (send.output) {
		if (obs_output_active(send.output)) {
			obs_output_force_stop(send.output);
		}
		obs_output_release(send.output);
		send.output = nullptr;
	}
	send.slot.Reset();
}

static void StartSend(CanvasSend &send, const char *ndiName, video_t *video, audio_t *audio)
{
	if (send.output && obs_output_active(send.output)) {
		return;
	}
	StopSend(send);
	if (!video || !audio) {
		CTD_NDI_LOG(LOG_WARNING, "'%s': nothing to send yet", ndiName);
		return;
	}
	send.slot = SlotGuard(&slots);
	if (!send.slot.Held()) {
		CTD_NDI_LOG(LOG_WARNING, "'%s' not started: %s (%d)", ndiName, SenderStateText(SenderState::Limit),
			    slots.Max());
		return;
	}
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "ndi_name", ndiName);
	const std::string outputName = std::string("CTD NDI send: ") + ndiName;
	send.output = obs_output_create("ctd_ndi_output", outputName.c_str(), settings, nullptr);
	obs_data_release(settings);
	if (!send.output) {
		CTD_NDI_LOG(LOG_ERROR, "'%s': the NDI output could not be created", ndiName);
		send.slot.Reset();
		return;
	}
	obs_output_set_media(send.output, video, audio);
	if (!obs_output_start(send.output)) {
		const char *error = obs_output_get_last_error(send.output);
		CTD_NDI_LOG(LOG_WARNING, "'%s' did not start%s%s", ndiName, error ? ": " : "", error ? error : "");
		StopSend(send);
	}
}

static video_t *VerticalVideo()
{
	obs_canvas_t *canvas = obs_get_canvas_by_name(kVerticalCanvas);
	if (!canvas) {
		return nullptr;
	}
	video_t *video = obs_canvas_get_video(canvas);
	obs_canvas_release(canvas);
	return video;
}

/* Starts or stops every sender to match the mode, the settings and what OBS is doing. UI thread. */
static void ReconcileSends()
{
	const bool allowed = loadingFinished && !switching.load() && Running();
	/* stops first, so their slots are free for what starts */
	if (!(allowed && sendProgram)) {
		StopSend(programSend);
	}
	if (!(allowed && sendVertical)) {
		StopSend(verticalSend);
	}
	if (!allowed) {
		FiltersStopAll();
		return;
	}
	if (sendProgram) {
		StartSend(programSend, kProgramName, obs_get_video(), obs_get_audio());
	}
	if (sendVertical) {
		StartSend(verticalSend, kVerticalName, VerticalVideo(), obs_get_audio());
	}
	FiltersReconcileAll();
}

static void StopEverything()
{
	StopSend(programSend);
	StopSend(verticalSend);
	FiltersStopAll();
}

/* ------------------------------------------------------------------------------------------------ apply */

static void Apply()
{
	const Mode mode = EffectiveMode(configuredMode, launchDisabled, obs_frontend_is_safe_mode_enabled());

	if (ModeSends(mode) && !Lib()) {
		if (InstalledVersion().empty()) {
			/* the first time NDI is switched on: set up from the NDI Runtime if it's there and approved */
			std::string message;
			SetupFromRuntime(message);
			lastMessage = message;
		}
		const LoadState state = LoadNdiLibrary();
		lastMessage = state.loaded ? std::string() : state.message;
	}

	if (currentMode.exchange(mode) != mode) {
		CTD_NDI_LOG(LOG_INFO, "NDI mode %s%s", ModeName(mode),
			    mode != configuredMode ? " (off for this launch)" : "");
	}
	runGeneration++;
	ReconcileSends();
}

/* ------------------------------------------------------------------------------------------------ procs */

static void ProcApply(void *, calldata_t *)
{
	ReadConfig();
	Apply();
}

static void ProcSetup(void *, calldata_t *cd)
{
	std::string message;
	const bool ok = SetupFromRuntime(message);
	lastMessage = message;
	if (ok) {
		ReadConfig();
		Apply();
		const LoadState state = CurrentLoadState();
		if (ModeSends(currentMode.load()) && !state.loaded) {
			message += " " + state.message;
		}
	}
	calldata_set_bool(cd, "ok", ok);
	calldata_set_string(cd, "message", message.c_str());
}

static void ProcPause(void *, calldata_t *cd)
{
	const bool want = calldata_bool(cd, "paused");
	if (paused.exchange(want) != want) {
		CTD_NDI_LOG(LOG_INFO, "NDI %s", want ? "paused: every sender stopped" : "resumed");
	}
	runGeneration++;
	ReconcileSends();
}

static void ProcStatus(void *, calldata_t *cd)
{
	const LoadState state = CurrentLoadState();
	std::string pieces;
	for (const std::string &piece : SystemPiecesFound()) {
		if (!pieces.empty()) {
			pieces += "\n";
		}
		pieces += piece;
	}
	calldata_set_string(cd, "mode", ModeName(currentMode.load()));
	calldata_set_string(cd, "configured", ModeName(configuredMode));
	calldata_set_bool(cd, "paused", paused.load());
	calldata_set_bool(cd, "launch_disabled", launchDisabled || obs_frontend_is_safe_mode_enabled());
	calldata_set_bool(cd, "loaded", state.loaded);
	calldata_set_string(cd, "version", state.version.c_str());
	calldata_set_string(cd, "installed", InstalledVersion().c_str());
	calldata_set_string(cd, "message", (lastMessage.empty() ? state.message : lastMessage).c_str());
	calldata_set_int(cd, "senders", slots.InUse());
	calldata_set_int(cd, "max_senders", slots.Max());
	calldata_set_int(cd, "dropped", static_cast<long long>(Sender::DroppedTotal()));
	calldata_set_bool(cd, "program", programSend.output && obs_output_active(programSend.output));
	calldata_set_bool(cd, "vertical", verticalSend.output && obs_output_active(verticalSend.output));
	calldata_set_string(cd, "system_pieces", pieces.c_str());
}

static void ProcFilterState(void *, calldata_t *cd)
{
	auto *filter = static_cast<obs_source_t *>(calldata_ptr(cd, "filter"));
	calldata_set_string(cd, "state", FilterState(filter).c_str());
}

/* ------------------------------------------------------------------------------------------------ frontend events */

static void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		loadingFinished = true;
		switching = false;
		runGeneration++;
		/* on the next turn of the UI loop: the vertical canvas creates its canvas in its own FINISHED_LOADING */
		obs_queue_task(OBS_TASK_UI, [](void *) { ReconcileSends(); }, nullptr, false);
		break;
	/* a profile or collection switch may reset video, which OBS refuses while an output is active */
	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		switching = true;
		StopEverything();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		switching = false;
		runGeneration++;
		ReconcileSends();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		loadingFinished = false;
		switching = true; /* nothing starts again */
		StopEverything();
		break;
	default:
		break;
	}
}

} // namespace ctd_ndi

using namespace ctd_ndi;

bool obs_module_load(void)
{
	RegisterOutputs();
	RegisterFilter();

	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph, "void ctd_ndi_apply()", ProcApply, nullptr);
	proc_handler_add(ph, "void ctd_ndi_setup(out bool ok, out string message)", ProcSetup, nullptr);
	proc_handler_add(ph, "void ctd_ndi_pause(in bool paused)", ProcPause, nullptr);
	proc_handler_add(ph,
			 "void ctd_ndi_status(out string mode, out string configured, out bool paused, "
			 "out bool launch_disabled, out bool loaded, out string version, out string installed, "
			 "out string message, out int senders, out int max_senders, out int dropped, out bool program, "
			 "out bool vertical, out string system_pieces)",
			 ProcStatus, nullptr);
	proc_handler_add(ph, "void ctd_ndi_filter_state(in ptr filter, out string state)", ProcFilterState, nullptr);

	calldata_t cd = {};
	if (proc_handler_call(ph, "ctd_ndi_launch_disabled", &cd)) {
		launchDisabled = calldata_bool(&cd, "disabled");
	}
	calldata_free(&cd);

	obs_frontend_add_event_callback(OnFrontendEvent, nullptr);

	ReadConfig();
	CTD_NDI_LOG(LOG_INFO, "NDI setting: %s%s%s", ModeName(configuredMode),
		    launchDisabled ? "; started with --ctd-no-ndi" : "",
		    obs_frontend_is_safe_mode_enabled() ? "; safe mode" : "");
	return true;
}

void obs_module_post_load(void)
{
	/* loads the NDI library only if the mode needs it */
	Apply();
}

void obs_module_unload(void)
{
	obs_frontend_remove_event_callback(OnFrontendEvent, nullptr);
	switching = true;
	StopEverything();
}
