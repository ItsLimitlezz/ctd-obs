/******************************************************************************
    CTD OBS: NDI in the app (docs/ctd/ndi.md).

      --ctd-no-ndi        NDI off for this launch (obs-main.cpp). The module
                          asks through ctd_ndi_launch_disabled, registered
                          here before modules load.
      CTD ▸ NDI           Pause NDI / Resume NDI (no restart, setting kept),
                          Start without NDI… (restart with --ctd-no-ndi),
                          NDI Settings…
      memory watchdog     started here (ctd-memory-watchdog.cpp)

    Global procs (obs_get_proc_handler()):

      void ctd_ndi_launch_disabled(out bool disabled)
          true for a --ctd-no-ndi launch.

    Called here, registered by the NDI module (plugins/ctd-ndi/src/ndi-plugin.cpp):
    ctd_ndi_status, ctd_ndi_apply, ctd_ndi_setup, ctd_ndi_pause.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ctd-ndi.hpp"
#include "ctd-memory-watchdog.hpp"

#include <widgets/OBSBasic.hpp>

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QMenu>
#include <QMessageBox>

extern bool restart;
extern bool ctd_opt_no_ndi;
extern bool ctd_restart_no_ndi;

static const char *Str(calldata_t *cd, const char *name)
{
	const char *value = calldata_string(cd, name);
	return value ? value : "";
}

CtdNdiStatus CtdNdiQueryStatus()
{
	CtdNdiStatus status;
	calldata_t cd = {};
	if (proc_handler_call(obs_get_proc_handler(), "ctd_ndi_status", &cd)) {
		status.available = true;
		status.mode = Str(&cd, "mode");
		status.configured = Str(&cd, "configured");
		status.paused = calldata_bool(&cd, "paused");
		status.launchDisabled = calldata_bool(&cd, "launch_disabled");
		status.loaded = calldata_bool(&cd, "loaded");
		status.version = Str(&cd, "version");
		status.installed = Str(&cd, "installed");
		status.message = Str(&cd, "message");
		status.receivers = static_cast<int>(calldata_int(&cd, "receivers"));
		status.maxReceivers = static_cast<int>(calldata_int(&cd, "max_receivers"));
		status.sends = static_cast<int>(calldata_int(&cd, "sends"));
		status.dropped = calldata_int(&cd, "dropped");
		status.systemPieces = Str(&cd, "system_pieces");
	}
	calldata_free(&cd);
	return status;
}

void CtdNdiApply()
{
	calldata_t cd = {};
	proc_handler_call(obs_get_proc_handler(), "ctd_ndi_apply", &cd);
	calldata_free(&cd);
}

bool CtdNdiSetup(std::string &message)
{
	calldata_t cd = {};
	bool ok = false;
	if (proc_handler_call(obs_get_proc_handler(), "ctd_ndi_setup", &cd)) {
		ok = calldata_bool(&cd, "ok");
		message = Str(&cd, "message");
	} else {
		message = QT_TO_UTF8(QTStr("CTD.NDI.NoModule"));
	}
	calldata_free(&cd);
	return ok;
}

void CtdNdiSetPaused(bool paused)
{
	calldata_t cd = {};
	calldata_set_bool(&cd, "paused", paused);
	proc_handler_call(obs_get_proc_handler(), "ctd_ndi_pause", &cd);
	calldata_free(&cd);
}

void CtdNdiSetConfigDefaults()
{
	config_t *config = App()->GetUserConfig();
	if (!config) {
		return;
	}
	config_set_default_string(config, kCtdNdiSection, kCtdNdiKeyMode, "off");
	config_set_default_bool(config, kCtdNdiSection, kCtdNdiKeySendProgram, true);
	config_set_default_bool(config, kCtdNdiSection, kCtdNdiKeySendVertical, false);
	config_set_default_uint(config, kCtdNdiSection, kCtdNdiKeyWatchdogLimit, 6144);
}

static void CtdNdiLaunchDisabledProc(void *, calldata_t *cd)
{
	calldata_set_bool(cd, "disabled", ctd_opt_no_ndi);
}

void OBSBasic::CtdNdiInit()
{
	/* before modules load: the NDI module asks for it in obs_module_load */
	proc_handler_add(obs_get_proc_handler(), "void ctd_ndi_launch_disabled(out bool disabled)",
			 CtdNdiLaunchDisabledProc, nullptr);
	CtdNdiSetConfigDefaults();
	if (ctd_opt_no_ndi) {
		blog(LOG_INFO, "[ctd-ndi] Started with --ctd-no-ndi: NDI is off for this launch");
	}
	ctdWatchdog = new CtdMemoryWatchdog(this);
}

void OBSBasic::CtdNdiAddMenu(QMenu *menu)
{
	const CtdNdiStatus status = CtdNdiQueryStatus();
	if (!status.available) {
		return;
	}

	QMenu *ndi = menu->addMenu(QTStr("CTD.NDI.Menu"));
	const bool on = status.mode != "off";

	QAction *pause = ndi->addAction(status.paused ? QTStr("CTD.NDI.Menu.Resume") : QTStr("CTD.NDI.Menu.Pause"));
	pause->setEnabled(on && status.loaded);
	const bool resume = status.paused;
	connect(pause, &QAction::triggered, this, [resume]() { CtdNdiSetPaused(!resume); });

	QAction *without = ndi->addAction(QTStr("CTD.NDI.Menu.StartWithout"));
	without->setMenuRole(QAction::NoRole);
	without->setEnabled(!ctd_opt_no_ndi);
	connect(without, &QAction::triggered, this, &OBSBasic::CtdNdiStartWithout);

	QAction *settings = ndi->addAction(QTStr("CTD.NDI.Menu.Settings"));
	settings->setMenuRole(QAction::NoRole);
	connect(settings, &QAction::triggered, this, &OBSBasic::CtdOpenSettings);
}

void OBSBasic::CtdNdiStartWithout()
{
	QString text = QTStr("CTD.NDI.StartWithout.Confirm");
	if (CtdAnyOutputActive()) {
		text += "\n\n" + QTStr("CTD.NDI.StartWithout.Live");
	}
	QMessageBox::StandardButton button = OBSMessageBox::question(this, QTStr("CTD.NDI.StartWithout.Title"), text);
	if (button != QMessageBox::Yes) {
		return;
	}
	blog(LOG_INFO, "[ctd-ndi] Restarting without NDI (CTD > NDI > Start without NDI)");
	restart = true;
	ctd_restart_no_ndi = true;
	close();
}
