/******************************************************************************
    CTD OBS: one CTD settings folder, and CTD settings in profile export/import
    (integration gap 17).

    Global procs (obs_get_proc_handler()), registered before modules load:

      void ctd_config_dir(out string path)
          <config>/ctd-obs/ctd (portable mode included; created if needed),
          next to plugin_config. CTD modules keep their settings files here.

    Called here, registered by the CTD plugin (ctd-obs-plugin, src/obs/ctd-settings.c):

      void ctd_settings_export(in string dir, out bool success, out string message)
      void ctd_settings_import(in string dir, out bool success, out string message)
          Profile ▸ Export writes the CTD settings, without secrets (no
          pairing token, no server passwords), into <export>/<profile>/ctd;
          Profile ▸ Import reads them from <profile folder>/ctd when present.

    The vertical canvas still keeps its own plugin_config/vertical-canvas/
    config.json (its stream outputs hold stream keys, so it stays out of
    exports); moving it needs a ctd-vertical-canvas change.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include <widgets/OBSBasic.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>
#include <util/platform.h>

#include <filesystem>

static void CtdConfigDirProc(void *, calldata_t *cd)
{
	char path[512];
	if (GetAppConfigPath(path, sizeof(path), "ctd-obs/ctd") <= 0) {
		return;
	}
	os_mkdirs(path);
	calldata_set_string(cd, "path", path);
}

void OBSBasic::CtdConfigRegisterProcs()
{
	proc_handler_add(obs_get_proc_handler(), "void ctd_config_dir(out string path)", CtdConfigDirProc, nullptr);
}

/* Runs a CTD settings proc on a folder; returns its message ("" when no module handles it) */
static QString CtdSettingsCall(const char *proc, const std::filesystem::path &dir)
{
	calldata_t cd = {};
	const std::string dirString = dir.u8string();
	calldata_set_string(&cd, "dir", dirString.c_str());
	QString message;
	if (proc_handler_call(obs_get_proc_handler(), proc, &cd)) {
		const char *text = calldata_string(&cd, "message");
		message = text ? QT_UTF8(text) : QString();
	}
	calldata_free(&cd);
	return message;
}

void OBSBasic::CtdExportProfileSettings(const std::filesystem::path &profileExportDir)
{
	CtdSettingsCall("ctd_settings_export", profileExportDir / std::filesystem::u8path("ctd"));
}

void OBSBasic::CtdImportProfileSettings(const std::filesystem::path &profileSourceDir)
{
	const std::filesystem::path ctdDir = profileSourceDir / std::filesystem::u8path("ctd");
	std::error_code error;
	if (!std::filesystem::is_directory(ctdDir, error)) {
		return;
	}
	const QString message = CtdSettingsCall("ctd_settings_import", ctdDir);
	if (!message.isEmpty()) {
		OBSMessageBox::information(this, QTStr("Basic.MainMenu.Profile.Import"), message);
	}
}
