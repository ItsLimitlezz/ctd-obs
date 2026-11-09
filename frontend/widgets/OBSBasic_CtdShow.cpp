/******************************************************************************
    CTD OBS: Show Profiles support (Spike S1 prototype).

    Global procs:
      ctd_show_apply(in string collection, in string profile,
                     out bool success, out string error, out bool restart_required)
      ctd_show_refresh(out bool success, out string error)

    obs-websocket bridge (vendor "ctd-obs"): requests "show_apply" and
    "show_refresh" with the same fields, for testing from outside OBS.

    See docs/ctd/show-profiles-s1.md for the spec.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "OBSBasic.hpp"

#include <models/SceneCollection.hpp>

#include <qt-wrappers.hpp>

#include <array>
#include <filesystem>
#include <string>
#include <vector>

namespace {
constexpr std::string_view CtdScenesPath = "ctd-obs/basic/scenes";
constexpr std::string_view CtdProfilesPath = "ctd-obs/basic/profiles";

const std::array<std::pair<const char *, bool>, 4> CtdProfileFiles{{
	{"basic.ini", true},
	{"service.json", false},
	{"streamEncoder.json", false},
	{"recordEncoder.json", false},
}};

bool sameFile(const std::filesystem::path &a, const std::filesystem::path &b)
{
	std::error_code ec;
	return std::filesystem::exists(a, ec) && std::filesystem::exists(b, ec) &&
	       std::filesystem::equivalent(a, b, ec);
}

bool copyOver(const std::filesystem::path &from, const std::filesystem::path &to, std::string &error)
{
	if (sameFile(from, to)) {
		return true;
	}

	std::error_code ec;
	std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
	if (ec) {
		error = "copy failed: " + from.u8string() + " -> " + to.u8string() + ": " + ec.message();
		return false;
	}
	return true;
}

/* --- Minimal obs-websocket vendor bridge (same calls as obs-websocket-api.h) --- */

typedef void (*CtdWsRequestFn)(obs_data_t *request, obs_data_t *response, void *priv);

struct CtdWsRequestCallback {
	CtdWsRequestFn callback;
	void *priv_data;
};

proc_handler_t *ctdWsProcHandler()
{
	calldata_t cd = {0, 0, 0, 0};
	proc_handler_t *ph = nullptr;
	if (proc_handler_call(obs_get_proc_handler(), "obs_websocket_api_get_ph", &cd)) {
		ph = static_cast<proc_handler_t *>(calldata_ptr(&cd, "ph"));
	}
	calldata_free(&cd);
	return ph;
}

void ctdWsRegister(CtdWsRequestFn applyFn, CtdWsRequestFn refreshFn)
{
	proc_handler_t *ph = ctdWsProcHandler();
	if (!ph) {
		blog(LOG_INFO, "[CTD Show] obs-websocket not available, vendor requests not registered");
		return;
	}

	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "name", "ctd-obs");
	proc_handler_call(ph, "vendor_register", &cd);
	void *vendor = calldata_ptr(&cd, "vendor");
	calldata_free(&cd);
	if (!vendor) {
		blog(LOG_WARNING, "[CTD Show] obs-websocket vendor registration failed");
		return;
	}

	auto addRequest = [&](const char *type, CtdWsRequestFn fn) {
		calldata_t rcd = {0, 0, 0, 0};
		CtdWsRequestCallback cb = {fn, nullptr};
		calldata_set_string(&rcd, "type", type);
		calldata_set_ptr(&rcd, "callback", &cb);
		calldata_set_ptr(&rcd, "vendor", vendor);
		proc_handler_call(ph, "vendor_request_register", &rcd);
		if (!calldata_bool(&rcd, "success")) {
			blog(LOG_WARNING, "[CTD Show] Failed to register obs-websocket request '%s'", type);
		}
		calldata_free(&rcd);
	};

	addRequest("show_apply", applyFn);
	addRequest("show_refresh", refreshFn);
}

void ctdShowApplyProc(void *, calldata_t *cd)
{
	const char *collection = calldata_string(cd, "collection");
	const char *profile = calldata_string(cd, "profile");
	std::string collectionPath = collection ? collection : "";
	std::string profilePath = profile ? profile : "";

	bool success = false;
	bool restartRequired = false;
	std::string error;

	OBSBasic *main = OBSBasic::Get();
	if (!main) {
		error = "main window not available";
	} else {
		/* Outputs, sources and the Qt UI must be touched on the UI thread. */
		QMetaObject::invokeMethod(
			main,
			[&]() { success = main->CtdShowApply(collectionPath, profilePath, error, restartRequired); },
			WaitConnection());
	}

	calldata_set_bool(cd, "success", success);
	calldata_set_string(cd, "error", error.c_str());
	calldata_set_bool(cd, "restart_required", restartRequired);
}

void ctdShowRefreshProc(void *, calldata_t *cd)
{
	bool success = false;
	std::string error;

	OBSBasic *main = OBSBasic::Get();
	if (!main) {
		error = "main window not available";
	} else {
		QMetaObject::invokeMethod(main, [&]() { success = main->CtdShowRefresh(error); }, WaitConnection());
	}

	calldata_set_bool(cd, "success", success);
	calldata_set_string(cd, "error", error.c_str());
}

void ctdWsShowApply(obs_data_t *request, obs_data_t *response, void *)
{
	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "collection", obs_data_get_string(request, "collection"));
	calldata_set_string(&cd, "profile", obs_data_get_string(request, "profile"));
	proc_handler_call(obs_get_proc_handler(), "ctd_show_apply", &cd);
	obs_data_set_bool(response, "success", calldata_bool(&cd, "success"));
	obs_data_set_string(response, "error", calldata_string(&cd, "error"));
	obs_data_set_bool(response, "restart_required", calldata_bool(&cd, "restart_required"));
	calldata_free(&cd);
}

void ctdWsShowRefresh(obs_data_t *, obs_data_t *response, void *)
{
	calldata_t cd = {0, 0, 0, 0};
	proc_handler_call(obs_get_proc_handler(), "ctd_show_refresh", &cd);
	obs_data_set_bool(response, "success", calldata_bool(&cd, "success"));
	obs_data_set_string(response, "error", calldata_string(&cd, "error"));
	calldata_free(&cd);
}
} // namespace

/* ------------------------------------------------------------------------- */

bool OBSBasic::CtdAnyOutputActive()
{
	bool active = false;
	obs_enum_outputs(
		[](void *param, obs_output_t *output) {
			if (obs_output_active(output)) {
				*static_cast<bool *>(param) = true;
				return false;
			}
			return true;
		},
		&active);
	return active;
}

void OBSBasic::CtdUpdateSwitchMenus()
{
	const bool enabled = !CtdAnyOutputActive();

	for (QAction *action : ui->sceneCollectionMenu->actions()) {
		if (action->property("file_name").typeName() != nullptr) {
			action->setEnabled(enabled);
		}
	}
	ui->actionNewSceneCollection->setEnabled(enabled);
	ui->actionDupSceneCollection->setEnabled(enabled);
	ui->actionRenameSceneCollection->setEnabled(enabled);
	ui->actionImportSceneCollection->setEnabled(enabled);
	ui->actionRemoveSceneCollection->setEnabled(enabled && collections.size() > 1);

	for (QAction *action : ui->profileMenu->actions()) {
		if (action->property("file_name").typeName() != nullptr) {
			action->setEnabled(enabled);
		}
	}
	ui->actionNewProfile->setEnabled(enabled);
	ui->actionDupProfile->setEnabled(enabled);
	ui->actionRenameProfile->setEnabled(enabled);
	ui->actionImportProfile->setEnabled(enabled);
	ui->actionRemoveProfile->setEnabled(enabled && profiles.size() > 1);
}

void OBSBasic::CtdRegisterShowProcs()
{
	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph,
			 "void ctd_show_apply(in string collection, in string profile, out bool success, "
			 "out string error, out bool restart_required)",
			 ctdShowApplyProc, nullptr);
	proc_handler_add(ph, "void ctd_show_refresh(out bool success, out string error)", ctdShowRefreshProc, nullptr);

	ctdWsRegister(ctdWsShowApply, ctdWsShowRefresh);

	/* C2: plugin outputs (e.g. the vertical canvas) don't raise frontend events, so re-check on every open. */
	connect(ui->sceneCollectionMenu, &QMenu::aboutToShow, this, &OBSBasic::CtdUpdateSwitchMenus);
	connect(ui->profileMenu, &QMenu::aboutToShow, this, &OBSBasic::CtdUpdateSwitchMenus);

	blog(LOG_INFO, "[CTD Show] Registered ctd_show_apply and ctd_show_refresh");
}

bool OBSBasic::CtdShowRefresh(std::string &error)
{
	if (disableSaving || clearingFailed) {
		error = "busy: a scene collection is loading";
		return false;
	}

	RefreshProfiles(true);
	RefreshSceneCollections(true);
	OnEvent(OBS_FRONTEND_EVENT_PROFILE_LIST_CHANGED);
	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);
	return true;
}

bool OBSBasic::CtdShowApply(const std::string &collectionPath, const std::string &profilePath, std::string &error,
			    bool &restartRequired)
{
	restartRequired = false;

	if (collectionPath.empty() && profilePath.empty()) {
		error = "nothing to apply";
		return false;
	}
	if (disableSaving || clearingFailed) {
		error = "busy: a scene collection is loading";
		return false;
	}
	if (CtdAnyOutputActive()) {
		error = "outputs active";
		return false;
	}

	/* ---- Validate the staged files before touching anything ---- */

	std::filesystem::path stagedCollection;
	std::string collectionName;
	if (!collectionPath.empty()) {
		stagedCollection = std::filesystem::u8path(collectionPath);
		OBSDataAutoRelease data = obs_data_create_from_json_file(collectionPath.c_str());
		if (!data) {
			error = "collection file missing or not valid JSON: " + collectionPath;
			return false;
		}
		collectionName = obs_data_get_string(data, "name");
		if (collectionName.empty()) {
			error = "collection file has no \"name\": " + collectionPath;
			return false;
		}
	}

	std::filesystem::path stagedProfile;
	std::string profileName;
	if (!profilePath.empty()) {
		stagedProfile = std::filesystem::u8path(profilePath);
		ConfigFile staged;
		if (staged.Open((stagedProfile / "basic.ini").u8string().c_str(), CONFIG_OPEN_EXISTING) !=
		    CONFIG_SUCCESS) {
			error = "profile folder has no readable basic.ini: " + profilePath;
			return false;
		}
		const char *name = config_get_string(staged, "General", "Name");
		profileName = (name && *name) ? name : stagedProfile.filename().u8string();
	}

	/* ---- Work out the targets ---- */

	const std::string activeCollection{config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")};
	const bool collectionIsActive = !collectionName.empty() && collectionName == activeCollection;

	std::filesystem::path targetCollection;
	if (!collectionName.empty()) {
		if (auto existing = GetSceneCollectionByName(collectionName)) {
			targetCollection = existing->getFilePath();
		} else {
			targetCollection = App()->userScenesLocation / std::filesystem::u8path(CtdScenesPath) /
					   stagedCollection.filename();
			if (std::filesystem::exists(targetCollection)) {
				error = "file name already used by another scene collection: " +
					targetCollection.filename().u8string();
				return false;
			}
		}
	}

	std::filesystem::path targetProfile;
	if (!profileName.empty()) {
		if (auto existing = GetProfileByName(profileName)) {
			targetProfile = existing->path;
		} else {
			targetProfile = App()->userProfilesLocation / std::filesystem::u8path(CtdProfilesPath) /
					stagedProfile.filename();
			if (std::filesystem::exists(targetProfile)) {
				error = "folder name already used by another profile: " +
					targetProfile.filename().u8string();
				return false;
			}
		}
	}

	/* Switching to a different collection keeps stock behaviour: the outgoing collection is saved first.
	 * Re-applying the active collection must NOT save, or the in-memory state would overwrite the
	 * staged file. */
	if (!collectionName.empty() && !collectionIsActive) {
		SaveProjectNow();
	}

	/* From here on nothing may save the scene collection until the new data is loaded. */
	disableSaving++;
	projectChanged = false;

	auto finish = [&](bool ok) {
		projectChanged = false;
		disableSaving--;
		return ok;
	};

	/* ---- Profile first: video settings must be in place before the collection loads ---- */

	if (!profileName.empty()) {
		std::error_code ec;
		std::filesystem::create_directories(targetProfile, ec);
		for (const auto &[file, mandatory] : CtdProfileFiles) {
			const auto from = stagedProfile / file;
			const auto to = targetProfile / file;
			if (std::filesystem::exists(from)) {
				if (!copyOver(from, to, error)) {
					return finish(false);
				}
			} else if (!mandatory && !sameFile(stagedProfile, targetProfile)) {
				std::filesystem::remove(to, ec);
			}
		}

		RefreshProfileCache();
		auto profile = GetProfileByName(profileName);
		if (!profile) {
			error = "profile not found after copy: " + profileName;
			return finish(false);
		}

		std::vector<std::string> requirements;
		OnEvent(OBS_FRONTEND_EVENT_PROFILE_CHANGING);
		ActivateProfile(profile.value(), true, &requirements);
		restartRequired = !requirements.empty();
		blog(LOG_INFO, "[CTD Show] Applied profile '%s'%s", profileName.c_str(),
		     restartRequired ? " (audio change needs a restart)" : "");
	}

	/* ---- Collection ---- */

	if (!collectionName.empty()) {
		if (!copyOver(stagedCollection, targetCollection, error)) {
			return finish(false);
		}

		RefreshSceneCollectionCache();
		auto collection = GetSceneCollectionByName(collectionName);
		if (!collection) {
			error = "scene collection not found after copy: " + collectionName;
			return finish(false);
		}

		OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING);
		/* disableSaving > 0 here, so ActivateSceneCollection's save-on-switch is skipped. */
		ActivateSceneCollection(collection.value());
		blog(LOG_INFO, "[CTD Show] Applied scene collection '%s'%s", collectionName.c_str(),
		     collectionIsActive ? " (reloaded active)" : "");
	}

	return finish(!clearingFailed);
}
