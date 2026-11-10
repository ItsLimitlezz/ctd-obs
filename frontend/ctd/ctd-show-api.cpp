/******************************************************************************
    CTD OBS: Show Profiles core API (C1) and switch guard (C2).

    Global procs (obs_get_proc_handler()):

      void ctd_show_apply(in string collection, in string profile,
                          out bool success, out string error, out bool restart_required)
      void ctd_show_refresh(out bool success, out string error)

    obs-websocket vendor "ctd-obs" (for tests from outside OBS): requests
    "show_apply" {collection, profile} and "show_refresh" {}, which answer with
    the same fields as the procs.

    C2: OBS's own Scene Collection and Profile switching (menus, frontend API,
    obs-websocket) is refused while any output is active.

    Spec: docs/ctd/show-profiles-core.md

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include <widgets/OBSBasic.hpp>
#include <widgets/OBSBasicStatusBar.hpp>

#include <models/SceneCollection.hpp>

#include <qt-wrappers.hpp>

#include <QMenu>

#include <array>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {
constexpr std::string_view CtdScenesPath = "ctd-obs/basic/scenes";
constexpr std::string_view CtdProfilesPath = "ctd-obs/basic/profiles";
constexpr std::string_view CtdTmpSuffix = ".ctd-tmp";
constexpr const char *CtdNoticeProperty = "ctd_switch_notice";

/* Profile files a show profile may carry. basic.ini is required. */
constexpr std::array<std::pair<const char *, bool>, 4> CtdProfileFiles{{
	{"basic.ini", true},
	{"service.json", false},
	{"streamEncoder.json", false},
	{"recordEncoder.json", false},
}};

/* Errors with a fixed meaning for callers (see docs/ctd/show-profiles-core.md). */
constexpr const char *ErrNothing = "nothing to apply";
constexpr const char *ErrBusy = "busy: a scene collection is loading";
constexpr const char *ErrStarting = "busy: CTD OBS is still starting";
constexpr const char *ErrOutputs = "outputs active";

bool isInside(const fs::path &child, const fs::path &folder)
{
	std::error_code ec;
	const fs::path a = fs::weakly_canonical(child, ec);
	if (ec) {
		return false;
	}
	const fs::path b = fs::weakly_canonical(folder, ec);
	if (ec) {
		return false;
	}
	auto it = a.begin();
	for (const auto &part : b) {
		if (part.empty()) {
			continue;
		}
		if (it == a.end() || *it != part) {
			return false;
		}
		++it;
	}
	return true;
}

fs::path withSuffix(const fs::path &path, std::string_view suffix)
{
	fs::path result = path;
	result += fs::u8path(suffix);
	return result;
}

bool copyTo(const fs::path &from, const fs::path &to, std::string &error)
{
	std::error_code ec;
	fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
	if (ec) {
		error = "copy failed: " + from.u8string() + " -> " + to.u8string() + ": " + ec.message();
		return false;
	}
	return true;
}

bool moveOver(const fs::path &from, const fs::path &to, std::string &error)
{
	std::error_code ec;
	fs::rename(from, to, ec);
	if (ec) {
		error = "could not move into place: " + to.u8string() + ": " + ec.message();
		return false;
	}
	return true;
}

void removeQuietly(const fs::path &path)
{
	std::error_code ec;
	fs::remove(path, ec);
}

/* Optional: the CTD vertical canvas fork answers ctd_vertical_get_output. If it reports "active", treat the
 * vertical outputs as running even if libobs doesn't list them (obs_enum_outputs normally does). */
bool verticalOutputActive()
{
	calldata_t cd = {0, 0, 0, 0};
	bool active = false;
	if (proc_handler_call(obs_get_proc_handler(), "ctd_vertical_get_output", &cd)) {
		bool value = false;
		if (calldata_get_bool(&cd, "active", &value)) {
			active = value;
		}
	}
	calldata_free(&cd);
	return active;
}

/* --- Minimal obs-websocket vendor bridge (same calls as obs-websocket-api.h) --- */

typedef void (*CtdWsRequestFn)(obs_data_t *request, obs_data_t *response, void *priv);

struct CtdWsRequestCallback {
	CtdWsRequestFn callback;
	void *priv_data;
};

proc_handler_t *wsProcHandler()
{
	calldata_t cd = {0, 0, 0, 0};
	proc_handler_t *ph = nullptr;
	if (proc_handler_call(obs_get_proc_handler(), "obs_websocket_api_get_ph", &cd)) {
		ph = static_cast<proc_handler_t *>(calldata_ptr(&cd, "ph"));
	}
	calldata_free(&cd);
	return ph;
}

void wsShowApply(obs_data_t *request, obs_data_t *response, void *)
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

void wsShowRefresh(obs_data_t *, obs_data_t *response, void *)
{
	calldata_t cd = {0, 0, 0, 0};
	proc_handler_call(obs_get_proc_handler(), "ctd_show_refresh", &cd);
	obs_data_set_bool(response, "success", calldata_bool(&cd, "success"));
	obs_data_set_string(response, "error", calldata_string(&cd, "error"));
	calldata_free(&cd);
}

void wsRegister()
{
	proc_handler_t *ph = wsProcHandler();
	if (!ph) {
		blog(LOG_INFO, "[ctd-show] obs-websocket not loaded; vendor requests not registered");
		return;
	}

	calldata_t cd = {0, 0, 0, 0};
	calldata_set_string(&cd, "name", "ctd-obs");
	proc_handler_call(ph, "vendor_register", &cd);
	void *vendor = calldata_ptr(&cd, "vendor");
	calldata_free(&cd);
	if (!vendor) {
		blog(LOG_WARNING, "[ctd-show] obs-websocket vendor registration failed");
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
			blog(LOG_WARNING, "[ctd-show] obs-websocket request '%s' not registered", type);
		}
		calldata_free(&rcd);
	};

	addRequest("show_apply", wsShowApply);
	addRequest("show_refresh", wsShowRefresh);
}

/* --- Proc handlers: any thread; the work runs on the UI thread --- */

void procShowApply(void *, calldata_t *cd)
{
	const char *collection = calldata_string(cd, "collection");
	const char *profile = calldata_string(cd, "profile");
	const std::string collectionPath = collection ? collection : "";
	const std::string profilePath = profile ? profile : "";

	bool success = false;
	bool restartRequired = false;
	std::string error;

	OBSBasic *main = OBSBasic::Get();
	if (!main) {
		error = ErrStarting;
	} else {
		QMetaObject::invokeMethod(
			main,
			[&]() { success = main->CtdShowApply(collectionPath, profilePath, error, restartRequired); },
			WaitConnection());
	}

	if (success) {
		blog(LOG_INFO, "[ctd-show] ctd_show_apply done%s", restartRequired ? " (restart required)" : "");
	} else {
		blog(LOG_WARNING, "[ctd-show] ctd_show_apply refused: %s", error.c_str());
	}

	calldata_set_bool(cd, "success", success);
	calldata_set_string(cd, "error", error.c_str());
	calldata_set_bool(cd, "restart_required", restartRequired);
}

void procShowRefresh(void *, calldata_t *cd)
{
	bool success = false;
	std::string error;

	OBSBasic *main = OBSBasic::Get();
	if (!main) {
		error = ErrStarting;
	} else {
		QMetaObject::invokeMethod(main, [&]() { success = main->CtdShowRefresh(error); }, WaitConnection());
	}

	calldata_set_bool(cd, "success", success);
	calldata_set_string(cd, "error", error.c_str());
}
} // namespace

/* ------------------------------------------------------------------------- */
/* Setup                                                                       */

void OBSBasic::CtdShowRegisterProcs()
{
	/* Registered before modules load, so plugins can look for ctd_show_apply in obs_module_post_load.
	 * Calls made before the main window has finished loading are refused with "busy: …starting". */
	proc_handler_t *ph = obs_get_proc_handler();
	proc_handler_add(ph,
			 "void ctd_show_apply(in string collection, in string profile, out bool success, "
			 "out string error, out bool restart_required)",
			 procShowApply, nullptr);
	proc_handler_add(ph, "void ctd_show_refresh(out bool success, out string error)", procShowRefresh, nullptr);
}

void OBSBasic::CtdShowFinishLoading()
{
	ctdShowReady = true;

	wsRegister();

	/* C2: plugin outputs (the vertical canvas) raise no frontend events, so check whenever a menu opens. */
	auto addNotice = [this](QMenu *menu) {
		QAction *notice = new QAction(QTStr("CTD.Show.SwitchBlocked"), menu);
		notice->setProperty(CtdNoticeProperty, true);
		notice->setEnabled(false);
		notice->setVisible(false);
		QAction *first = menu->actions().isEmpty() ? nullptr : menu->actions().first();
		menu->insertAction(first, notice);

		connect(menu, &QMenu::aboutToShow, this, [this]() { CtdUpdateSwitchMenus(true); });
		connect(menu, &QMenu::aboutToHide, this, [this]() { CtdUpdateSwitchMenus(false); });
	};
	addNotice(ui->sceneCollectionMenu);
	addNotice(ui->profileMenu);

	blog(LOG_INFO, "[ctd-show] ctd_show_apply and ctd_show_refresh ready");
}

/* ------------------------------------------------------------------------- */
/* C2: output guard                                                            */

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
	return active || verticalOutputActive();
}

bool OBSBasic::CtdSwitchBlocked()
{
	if (!CtdAnyOutputActive()) {
		return false;
	}

	blog(LOG_INFO, "[ctd-show] Scene collection / profile switch refused: outputs active");
	if (ui && ui->statusbar) {
		ui->statusbar->showMessage(QTStr("CTD.Show.SwitchBlocked"), 5000);
	}
	return true;
}

void OBSBasic::CtdUpdateSwitchMenus(bool menuOpen)
{
	/* Items are only disabled while a menu is open. Closed, they stay enabled so that
	 * obs_frontend_set_current_* (which triggers the menu action) reaches the guard in
	 * ChangeSceneCollection / ChangeProfile instead of being silently ignored. */
	const bool blocked = menuOpen && CtdAnyOutputActive();
	const bool enabled = !blocked;

	auto update = [blocked, enabled](QMenu *menu) {
		for (QAction *action : menu->actions()) {
			if (action->property(CtdNoticeProperty).toBool()) {
				action->setVisible(blocked);
			} else if (action->property("file_name").typeName() != nullptr) {
				action->setEnabled(enabled);
			}
		}
	};
	update(ui->sceneCollectionMenu);
	update(ui->profileMenu);

	/* Export stays enabled: it doesn't switch anything. Profile import only copies a folder. */
	ui->actionNewSceneCollection->setEnabled(enabled);
	ui->actionDupSceneCollection->setEnabled(enabled);
	ui->actionRenameSceneCollection->setEnabled(enabled);
	ui->actionImportSceneCollection->setEnabled(enabled);
	ui->actionRemoveSceneCollection->setEnabled(enabled && collections.size() > 1);

	ui->actionNewProfile->setEnabled(enabled);
	ui->actionDupProfile->setEnabled(enabled);
	ui->actionRenameProfile->setEnabled(enabled);
	ui->actionRemoveProfile->setEnabled(enabled && profiles.size() > 1);
}

/* ------------------------------------------------------------------------- */
/* C1: ctd_show_refresh                                                        */

bool OBSBasic::CtdShowRefresh(std::string &error)
{
	if (!ctdShowReady) {
		error = ErrStarting;
		return false;
	}
	if (ctdShowApplying || disableSaving || clearingFailed) {
		error = ErrBusy;
		return false;
	}

	RefreshProfiles(true);

	/* The rescan only sees collections that are on disk and builds new entries for them. On a fresh
	 * config the active collection ("Untitled") exists only in memory until OBS's first deferred
	 * save, and the CTD plugin probes for this proc during FINISHED_LOADING, before that save runs.
	 * Dropping the entry would make every later save fail ("Scene collection not found in collection
	 * list"), so the active collection stays listed. Its in-memory state (coordinate mode, migration
	 * resolution, which Save() writes) is kept too. */
	std::optional<OBS::SceneCollection> active;
	try {
		active = GetCurrentSceneCollection();
	} catch (const std::invalid_argument &) {
	}

	RefreshSceneCollectionCache();
	if (active) {
		auto found = collections.find(active->getName());
		if (found == collections.end()) {
			blog(LOG_INFO, "[ctd-show] Keeping unsaved active scene collection '%s' (%s) in the list",
			     active->getName().c_str(), active->getFileName().c_str());
			collections.try_emplace(active->getName(), std::move(*active));
		} else if (found->second.getFilePath() == active->getFilePath()) {
			found->second = std::move(*active);
		}
	}
	RefreshSceneCollections();

	OnEvent(OBS_FRONTEND_EVENT_PROFILE_LIST_CHANGED);
	OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_LIST_CHANGED);
	return true;
}

/* ------------------------------------------------------------------------- */
/* C1: ctd_show_apply                                                          */

bool OBSBasic::CtdShowApply(const std::string &collectionPath, const std::string &profilePath, std::string &error,
			    bool &restartRequired)
{
	restartRequired = false;
	error.clear();

	if (!ctdShowReady) {
		error = ErrStarting;
		return false;
	}
	if (collectionPath.empty() && profilePath.empty()) {
		error = ErrNothing;
		return false;
	}
	if (ctdShowApplying || disableSaving || clearingFailed) {
		error = ErrBusy;
		return false;
	}
	if (CtdAnyOutputActive()) {
		error = ErrOutputs;
		return false;
	}

	const fs::path scenesDir = App()->userScenesLocation / fs::u8path(CtdScenesPath);
	const fs::path profilesDir = App()->userProfilesLocation / fs::u8path(CtdProfilesPath);

	/* ---- 1. Validate the staged files. Nothing is changed yet. ---- */

	fs::path stagedCollection;
	std::string collectionName;
	if (!collectionPath.empty()) {
		stagedCollection = fs::u8path(collectionPath);
		if (!fs::is_regular_file(stagedCollection)) {
			error = "collection file not found: " + collectionPath;
			return false;
		}
		if (isInside(stagedCollection, scenesDir)) {
			error = "collection file must be staged outside basic/scenes: " + collectionPath;
			return false;
		}
		OBSDataAutoRelease stagedData = obs_data_create_from_json_file(collectionPath.c_str());
		if (!stagedData) {
			error = "collection file is not valid JSON: " + collectionPath;
			return false;
		}
		collectionName = obs_data_get_string(stagedData, "name");
		if (collectionName.empty()) {
			error = "collection file has no \"name\": " + collectionPath;
			return false;
		}
	}

	fs::path stagedProfile;
	std::string profileName;
	if (!profilePath.empty()) {
		stagedProfile = fs::u8path(profilePath);
		if (!stagedProfile.has_filename()) {
			stagedProfile = stagedProfile.parent_path();
		}
		if (!fs::is_directory(stagedProfile)) {
			error = "profile folder not found: " + profilePath;
			return false;
		}
		if (isInside(stagedProfile, profilesDir)) {
			error = "profile folder must be staged outside basic/profiles: " + profilePath;
			return false;
		}
		ConfigFile staged;
		if (staged.Open((stagedProfile / "basic.ini").u8string().c_str(), CONFIG_OPEN_EXISTING) !=
		    CONFIG_SUCCESS) {
			error = "profile folder has no readable basic.ini: " + profilePath;
			return false;
		}
		const char *name = config_get_string(staged, "General", "Name");
		profileName = (name && *name) ? name : stagedProfile.filename().u8string();
	}

	ctdShowApplying = true;
	struct ApplyingReset {
		bool &flag;
		~ApplyingReset() { flag = false; }
	} applyingReset{ctdShowApplying};

	const std::string activeCollection{config_get_string(App()->GetUserConfig(), "Basic", "SceneCollection")};
	const std::string activeProfile{config_get_string(App()->GetUserConfig(), "Basic", "Profile")};

	/* ---- 2. Resolve targets and copy the staged files next to them (*.ctd-tmp). ----
	 * OBS's state is still untouched, so any failure here leaves everything as it was. */

	std::optional<SceneCollection> targetCollection;
	bool collectionIsNew = false;
	fs::path collectionTmp;

	std::optional<OBSProfile> targetProfile;
	bool profileIsNew = false;
	std::vector<std::pair<fs::path, fs::path>> profileMoves; /* tmp -> final */
	std::vector<fs::path> profileRemovals;

	auto cleanup = [&]() {
		if (!collectionTmp.empty()) {
			removeQuietly(collectionTmp);
		}
		for (const auto &[tmp, _] : profileMoves) {
			removeQuietly(tmp);
		}
		if (collectionIsNew && targetCollection) {
			collections.erase(collectionName);
		}
		if (profileIsNew && targetProfile) {
			std::error_code ec;
			fs::remove_all(targetProfile->path, ec);
			profiles.erase(profileName);
			OnEvent(OBS_FRONTEND_EVENT_PROFILE_LIST_CHANGED);
		}
	};

	try {
		if (!collectionName.empty()) {
			if (auto existing = GetSceneCollectionByName(collectionName)) {
				targetCollection = existing.value();
			} else {
				targetCollection = CreateSceneCollection(collectionName);
				collectionIsNew = true;
			}
			collectionTmp = withSuffix(targetCollection->getFilePath(), CtdTmpSuffix);
			if (!copyTo(stagedCollection, collectionTmp, error)) {
				cleanup();
				return false;
			}
		}

		if (!profileName.empty()) {
			if (auto existing = GetProfileByName(profileName)) {
				targetProfile = existing.value();
			} else {
				targetProfile = CreateProfile(profileName);
				profileIsNew = true;
			}
			for (const auto &[file, required] : CtdProfileFiles) {
				const fs::path from = stagedProfile / file;
				const fs::path to = targetProfile->path / file;
				if (fs::exists(from)) {
					const fs::path tmp = withSuffix(to, CtdTmpSuffix);
					profileMoves.emplace_back(tmp, to);
					if (!copyTo(from, tmp, error)) {
						cleanup();
						return false;
					}
				} else if (!required) {
					profileRemovals.push_back(to);
				}
			}
		}
	} catch (const std::exception &e) {
		error = e.what();
		cleanup();
		return false;
	}

	/* ---- 3. Profile first, so video settings are in place before the collection loads. ---- */

	if (targetProfile) {
		/* CHANGING first, then the files go into place: a plugin saving the profile config from its
		 * CHANGING callback writes the old config before the new files arrive, not over them. */
		OnEvent(OBS_FRONTEND_EVENT_PROFILE_CHANGING);

		for (const auto &[tmp, to] : profileMoves) {
			if (!moveOver(tmp, to, error)) {
				cleanup();
				return false;
			}
		}
		profileMoves.clear();
		for (const auto &path : profileRemovals) {
			removeQuietly(path);
		}

		try {
			std::vector<std::string> requirements;
			/* For a re-apply of the active profile, ActivateProfile reads the new basic.ini before it
			 * writes the old in-memory config, then saves the new one back, so the result is the new file. */
			ActivateProfile(targetProfile.value(), true, &requirements);
			restartRequired = !requirements.empty();
		} catch (const std::exception &e) {
			error = std::string("profile apply failed: ") + e.what();
			if (!collectionTmp.empty()) {
				removeQuietly(collectionTmp);
			}
			if (collectionIsNew) {
				collections.erase(collectionName);
			}
			return false;
		}

		blog(LOG_INFO, "[ctd-show] Applied profile '%s' (%s)%s%s", profileName.c_str(),
		     targetProfile->directoryName.c_str(), profileName == activeProfile ? ", reloaded active" : "",
		     restartRequired ? ", audio change needs a restart" : "");
	}

	/* ---- 4. Collection. ----
	 * CHANGING goes out first, then the staged file is moved over the target and loaded straight away,
	 * with no return to the event loop in between, so no queued or new save can write the in-memory
	 * scenes over it. Switching to another collection still saves the outgoing one to its own file
	 * (stock ActivateSceneCollection); re-applying the active one does not save at all. */

	if (targetCollection) {
		OnEvent(OBS_FRONTEND_EVENT_SCENE_COLLECTION_CHANGING);

		if (!moveOver(collectionTmp, targetCollection->getFilePath(), error)) {
			removeQuietly(collectionTmp);
			if (collectionIsNew) {
				collections.erase(collectionName);
			}
			error += targetProfile ? " (the profile was applied)" : "";
			return false;
		}
		collectionTmp.clear();

		/* Like stock new collections: a stale .bak left by a removed collection of the same file name
		 * must not be picked up. An existing collection keeps its .bak (OBS's own last-save backup). */
		if (collectionIsNew) {
			removeQuietly(withSuffix(targetCollection->getFilePath(), ".bak"));
		}

		try {
			auto collection = GetSceneCollectionByName(collectionName);
			if (!collection) {
				error = "scene collection not found after copy: " + collectionName;
				return false;
			}
			ActivateSceneCollection(collection.value());
		} catch (const std::exception &e) {
			error = std::string("scene collection load failed: ") + e.what();
			return false;
		}

		blog(LOG_INFO, "[ctd-show] Applied scene collection '%s' (%s)%s", collectionName.c_str(),
		     targetCollection->getFileName().c_str(),
		     collectionName == activeCollection ? ", reloaded active" : "");
	}

	if (clearingFailed) {
		error = "the old scene data could not be cleared; restart CTD OBS";
		return false;
	}

	return true;
}
