/******************************************************************************
    CTD OBS: the top-level CTD menu (integration gaps 3 and 19).

      CTD
        CTD Relay / CTD Relay · Feeds / Vertical
                                          the docks' own show/hide actions
        Feed Servers…                     the CTD plugin's feed servers window
                                          (incoming feeds are part of CTD Relay;
                                          there is no "CTD Hub")
        CTD Settings…                     OBS Settings, on its CTD page
        Check for Updates…                CTD OBS's update check (ctd-update.cpp)
        Advanced ▸ Plugin Manager… · Scripts… · WebSocket Server Settings…

    The menu is rebuilt each time it opens, so it follows whichever docks and
    tools the modules have registered (none of them are required).

    Global procs (obs_get_proc_handler()):

      void ctd_menu_available(out bool available)
          Registered before modules load. When it exists, the CTD plugin leaves
          its servers item out of Tools, since this menu has it.

    Called here, registered by the CTD plugin (ctd-obs-plugin, src/ui/servers-dialog.cpp):

      void ctd_servers_dialog_open()
          Opens the servers window. A plugin without it still has its Tools
          item, which Feed Servers… then triggers instead.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ctd-update-logic.hpp"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QDockWidget>
#include <QMenu>
#include <QMenuBar>

/* Dock ids of CTD OBS's built-in modules, in menu order. These are the modules' saved dock ids; never rename them
 * ("ctd-hub" is the CTD Relay · Feeds dock: the id is from before it was renamed, so saved layouts keep working;
 * "ctd_producer" is the CTD Producer dock: viewers per platform and the merged chat; "ctd-overlays" is the CTD Overlays
 * dock: CTD Overlay's overlays, template updates and the template kit, through CTD Relay). */
static const char *const kCtdMenuDocks[] = {"ctd-multistream", "ctd_producer", "ctd-overlays", "ctd-hub",
					    "VerticalCanvasDock"};

/* The CTD plugin's module (plugins/ctd-live-source) */
static const char *const kCtdPluginModule = "ctd-live-source";

/* The CTD plugin's Tools item, for a plugin without ctd_servers_dialog_open or one that still adds it (newest name
 * first; the older ones were hard-coded English) */
static const char *const kLegacyServersToolsItems[] = {"CTD Relay feed servers…", "CTD Hub servers…",
						       "CTD Live servers…"};

static void CtdMenuAvailableProc(void *, calldata_t *cd)
{
	calldata_set_bool(cd, "available", true);
}

/* A Tools menu action a module added, found by its text in that module's own translation */
static QAction *FindToolsAction(QMenu *tools, const char *moduleName, const char *lookup)
{
	QString text;
	if (moduleName) {
		obs_module_t *module = obs_get_module(moduleName);
		const char *translated = module ? obs_module_get_locale_text(module, lookup) : nullptr;
		if (!translated) {
			return nullptr;
		}
		text = QT_UTF8(translated);
	} else {
		text = QT_UTF8(lookup);
	}

	for (QAction *action : tools->actions()) {
		if (!action->isSeparator() && !action->menu() && action->text() == text) {
			return action;
		}
	}
	return nullptr;
}

void OBSBasic::CtdMenuInit()
{
	proc_handler_add(obs_get_proc_handler(), "void ctd_menu_available(out bool available)", CtdMenuAvailableProc,
			 nullptr);

	ctdMenu = new QMenu(QTStr("CTD.Menu"), ui->menubar);
	ctdMenu->setObjectName(QStringLiteral("menuCTD"));
	ui->menubar->insertMenu(ui->menuBasic_MainMenu_Help->menuAction(), ctdMenu);

	connect(ctdMenu.data(), &QMenu::aboutToShow, this, &OBSBasic::CtdMenuRebuild);
	CtdMenuRebuild();
}

void OBSBasic::CtdMenuRebuild()
{
	if (!ctdMenu) {
		return;
	}

	/* clear() deletes only the actions this menu owns (the ones created below), not the docks' or Tools'; the old
	 * Advanced submenu goes separately (it holds only borrowed actions) */
	for (QMenu *submenu : ctdMenu->findChildren<QMenu *>(Qt::FindDirectChildrenOnly)) {
		submenu->deleteLater();
	}
	ctdMenu->clear();

	for (const char *id : kCtdMenuDocks) {
		QDockWidget *dock = findChild<QDockWidget *>(QString::fromLatin1(id));
		if (dock) {
			ctdMenu->addAction(dock->toggleViewAction());
		}
	}

	ctdMenu->addSeparator();

	QAction *servers = ctdMenu->addAction(QTStr("CTD.Menu.Servers"));
	connect(servers, &QAction::triggered, this, [this]() {
		calldata_t cd = {};
		const bool opened = proc_handler_call(obs_get_proc_handler(), "ctd_servers_dialog_open", &cd);
		calldata_free(&cd);
		if (opened) {
			return;
		}
		for (const char *text : kLegacyServersToolsItems) {
			if (QAction *legacy = FindToolsAction(ui->menuTools, nullptr, text)) {
				legacy->trigger();
				return;
			}
		}
	});
	/* the plugin (its proc or, if older, its Tools item) must be there for the item to do anything */
	servers->setEnabled(obs_get_module(kCtdPluginModule) != nullptr);

	QAction *settings = ctdMenu->addAction(QTStr("CTD.Menu.Settings"));
	settings->setMenuRole(QAction::NoRole);
	connect(settings, &QAction::triggered, this, &OBSBasic::CtdOpenSettings);

	/* frontend/ctd/ctd-update.cpp: CTD OBS's own update check (Windows and macOS builds) */
	if (CtdCurrentPlatform() != CtdUpdatePlatform::Other) {
		QAction *updates = ctdMenu->addAction(QTStr("CTD.Menu.CheckForUpdates"));
		updates->setMenuRole(QAction::NoRole);
		connect(updates, &QAction::triggered, this, &OBSBasic::CtdCheckForUpdates);
	}

	/* Gap 19: OBS's own advanced tools, linked here so CTD users find them next to the CTD items */
	QMenu *advanced = ctdMenu->addMenu(QTStr("CTD.Menu.Advanced"));
	advanced->addAction(ui->actionOpenPluginManager);
	if (QAction *scripts = FindToolsAction(ui->menuTools, "frontend-tools", "Scripts")) {
		advanced->addAction(scripts);
	}
	if (QAction *websocket = FindToolsAction(ui->menuTools, "obs-websocket", "OBSWebSocket.Settings.DialogTitle")) {
		advanced->addAction(websocket);
	}
}

void OBSBasic::CtdOpenSettings()
{
	/* One-shot: the next OBSBasicSettings opens on its CTD page and clears it (frontend/ctd/ctd-settings-page.cpp).
	 * Not cleared here, because on_action_Settings_triggered may defer itself by a second (inside an event loop). */
	ctdSettingsOpenOnCtdPage = true;
	on_action_Settings_triggered();
}
