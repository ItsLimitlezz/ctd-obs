/******************************************************************************
    CTD OBS NDI module (ctd-ndi): NDI® receive and send inside CTD OBS's own
    process, with no system pieces (docs/ctd/ndi.md).

    Mode ([CTD] NDIMode in user.ini): off (default) / receive / receive_send.
    While off, the NDI library isn't loaded. --ctd-no-ndi and OBS's safe
    mode force off for the launch. Switching to off stops every receiver,
    the finder and the sends at once.

    Global procs (obs_get_proc_handler()), for the CTD OBS app:

      void ctd_ndi_apply()
          Reads [CTD] NDIMode / NDISendProgram / NDISendVertical and applies
          them now (Settings ▸ CTD ▸ NDI, OK/Apply).
      void ctd_ndi_setup(out bool ok, out string message)
          Set up from the installed NDI 6 Runtime (allow-listed SHA-256
          only), then applies the mode.
      void ctd_ndi_pause(in bool paused)
          Pauses / resumes all receivers and sends without changing the
          setting (CTD ▸ NDI, the memory watchdog).
      void ctd_ndi_status(out string mode, out string configured,
                          out bool paused, out bool launch_disabled,
                          out bool loaded, out string version,
                          out string installed, out string message,
                          out int receivers, out int max_receivers,
                          out int sends, out int dropped,
                          out string system_pieces)

    Called here, registered by the CTD OBS app before modules load:

      void ctd_ndi_launch_disabled(out bool disabled)
          true for a --ctd-no-ndi launch

    Copyright (C) 2026 CineTech Digital

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ndi-module.hpp"

#include <obs-frontend-api.h>
#include <util/config-file.h>

#include <atomic>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("ctd-ndi", "en-US")

MODULE_EXPORT const char *obs_module_description(void)
{
	return "NDI receive and send for CTD OBS";
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
static std::atomic<uint64_t> runGeneration{0};
static bool launchDisabled = false;
static bool sendProgram = true;
static bool sendVertical = false;
static std::string lastMessage;

/* UI thread only */
static bool loadingFinished = false;
static bool switching = false;
static obs_output_t *programOutput = nullptr;
static obs_output_t *verticalOutput = nullptr;

static ReceiverSlots slots(kMaxReceivers);

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
	return Lib() != nullptr && ModeReceives(currentMode.load()) && !paused.load();
}

bool SendingAllowed()
{
	return Running() && ModeSends(currentMode.load());
}

uint64_t RunGeneration()
{
	return runGeneration.load();
}

ReceiverSlots &Slots()
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

/* ------------------------------------------------------------------------------------------------ sends */

static void StopSend(obs_output_t *&output)
{
	if (!output) {
		return;
	}
	if (obs_output_active(output)) {
		obs_output_force_stop(output);
	}
	obs_output_release(output);
	output = nullptr;
}

static void StartSend(obs_output_t *&output, const char *ndiName, video_t *video, audio_t *audio)
{
	if (output && obs_output_active(output)) {
		return;
	}
	if (!video || !audio) {
		CTD_NDI_LOG(LOG_WARNING, "'%s': nothing to send yet", ndiName);
		return;
	}
	if (!output) {
		obs_data_t *settings = obs_data_create();
		obs_data_set_string(settings, "ndi_name", ndiName);
		const std::string outputName = std::string("CTD NDI send: ") + ndiName;
		output = obs_output_create("ctd_ndi_output", outputName.c_str(), settings, nullptr);
		obs_data_release(settings);
		if (!output) {
			CTD_NDI_LOG(LOG_ERROR, "'%s': the NDI output could not be created", ndiName);
			return;
		}
	}
	obs_output_set_media(output, video, audio);
	if (!obs_output_start(output)) {
		const char *error = obs_output_get_last_error(output);
		CTD_NDI_LOG(LOG_WARNING, "'%s' did not start%s%s", ndiName, error ? ": " : "", error ? error : "");
		obs_output_release(output);
		output = nullptr;
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

/* Starts or stops the sends to match the mode, the settings and what OBS is doing. UI thread. */
static void ReconcileSends()
{
	const bool allowed = loadingFinished && !switching && SendingAllowed();
	if (allowed && sendProgram) {
		StartSend(programOutput, kProgramName, obs_get_video(), obs_get_audio());
	} else {
		StopSend(programOutput);
	}
	if (allowed && sendVertical) {
		StartSend(verticalOutput, kVerticalName, VerticalVideo(), obs_get_audio());
	} else {
		StopSend(verticalOutput);
	}
}

/* ------------------------------------------------------------------------------------------------ apply */

static void Apply()
{
	const Mode mode = EffectiveMode(configuredMode, launchDisabled, obs_frontend_is_safe_mode_enabled());

	if (ModeReceives(mode) && !Lib()) {
		if (InstalledVersion().empty()) {
			/* the first time NDI is switched on: set up from the NDI Runtime if it's there and approved */
			std::string message;
			SetupFromRuntime(message);
			lastMessage = message;
		}
		const LoadState state = LoadNdiLibrary();
		if (!state.loaded) {
			lastMessage = state.message;
		} else {
			lastMessage.clear();
		}
	}

	if (currentMode.exchange(mode) != mode) {
		CTD_NDI_LOG(LOG_INFO, "NDI mode %s%s", ModeName(mode),
			    mode != configuredMode ? " (off for this launch)" : "");
	}
	runGeneration++;
	if (!Running()) {
		FinderStop();
	}
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
		if (ModeReceives(currentMode.load()) && !state.loaded) {
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
		CTD_NDI_LOG(LOG_INFO, "NDI %s", want ? "paused: receivers and sends stopped" : "resumed");
	}
	runGeneration++;
	if (want) {
		FinderStop();
	}
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
	calldata_set_int(cd, "receivers", Running() ? slots.InUse() : 0);
	calldata_set_int(cd, "max_receivers", slots.Max());
	calldata_set_int(cd, "sends", ActiveSends());
	calldata_set_int(cd, "dropped", static_cast<long long>(DroppedSendFrames()));
	calldata_set_string(cd, "system_pieces", pieces.c_str());
}

/* ------------------------------------------------------------------------------------------------ frontend events */

static void OnFrontendEvent(enum obs_frontend_event event, void *)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_FINISHED_LOADING:
		loadingFinished = true;
		/* on the next turn of the UI loop: the vertical canvas creates its canvas in its own FINISHED_LOADING */
		obs_queue_task(OBS_TASK_UI, [](void *) { ReconcileSends(); }, nullptr, false);
		break;
	/* a profile or collection switch may reset video, which OBS refuses while an output is active */
	case OBS_FRONTEND_EVENT_PROFILE_CHANGING:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING:
		switching = true;
		ReconcileSends();
		break;
	case OBS_FRONTEND_EVENT_PROFILE_CHANGED:
	case OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGED:
		switching = false;
		ReconcileSends();
		break;
	case OBS_FRONTEND_EVENT_EXIT:
		loadingFinished = false;
		StopSend(programOutput);
		StopSend(verticalOutput);
		FinderStop();
		break;
	default:
		break;
	}
}

} // namespace ctd_ndi

using namespace ctd_ndi;

bool obs_module_load(void)
{
	RegisterSource();
	RegisterOutput();

	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph, "void ctd_ndi_apply()", ProcApply, nullptr);
	proc_handler_add(ph, "void ctd_ndi_setup(out bool ok, out string message)", ProcSetup, nullptr);
	proc_handler_add(ph, "void ctd_ndi_pause(in bool paused)", ProcPause, nullptr);
	proc_handler_add(
		ph,
		"void ctd_ndi_status(out string mode, out string configured, out bool paused, "
		"out bool launch_disabled, out bool loaded, out string version, out string installed, "
		"out string message, out int receivers, out int max_receivers, out int sends, out int dropped, "
		"out string system_pieces)",
		ProcStatus, nullptr);

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
	StopSend(programOutput);
	StopSend(verticalOutput);
	FinderStop();
}
