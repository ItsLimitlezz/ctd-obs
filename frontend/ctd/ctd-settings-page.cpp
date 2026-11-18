/******************************************************************************
    CTD OBS: the Settings ▸ CTD page (integration gaps 7 and 19).

      CTD Relay + CTD Hub   the CTD plugin's section (connection, pairing,
                            profile, WebSocket sharing, Hub servers)
      Vertical              the vertical canvas's own settings window
      Advanced              Plugin Manager, Scripts, WebSocket Server
                            Settings (they open once Settings closes)

    The docks keep the live status and the show actions. The page is the
    last one in the list, so OBS's own pages keep their indexes.

    Called here, registered by the CTD plugin (ctd-obs-plugin, src/ui/settings-section.cpp):

      void ctd_settings_widget(in ptr parent, out ptr widget)
          The plugin's section as a child of parent. Without it the page
          points at the CTD Relay dock instead.

    The vertical canvas has no call for its settings window, so the page
    presses the vertical dock's own settings (gear) button.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "settings/OBSBasicSettings.hpp"
#include "ui_OBSBasicSettings.h"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <QDockWidget>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QStackedWidget>
#include <QVBoxLayout>

/* Dock ids of CTD OBS's built-in modules (their saved dock ids; never rename them) */
static const char *const kCtdRelayDockId = "ctd-multistream";
static const char *const kCtdVerticalDockId = "VerticalCanvasDock";

/* A Tools menu action a module added, found by its text in that module's own translation */
static QAction *CtdFindModuleToolsAction(QMenu *tools, const char *moduleName, const char *lookup)
{
	obs_module_t *module = obs_get_module(moduleName);
	const char *translated = module ? obs_module_get_locale_text(module, lookup) : nullptr;
	if (!tools || !translated) {
		return nullptr;
	}
	const QString text = QT_UTF8(translated);
	for (QAction *action : tools->actions()) {
		if (!action->isSeparator() && !action->menu() && action->text() == text) {
			return action;
		}
	}
	return nullptr;
}

/* The vertical dock's settings (gear) button: the vertical canvas offers no other way to open its settings window */
static QPushButton *CtdVerticalSettingsButton(QWidget *mainWindow)
{
	QDockWidget *dock = mainWindow ? mainWindow->findChild<QDockWidget *>(QString::fromLatin1(kCtdVerticalDockId))
				       : nullptr;
	QWidget *canvas = dock ? dock->widget() : nullptr;
	if (!canvas) {
		return nullptr;
	}
	for (QPushButton *button : canvas->findChildren<QPushButton *>()) {
		if (button->property("themeID").toString() == QLatin1String("configIconSmall")) {
			return button;
		}
	}
	return nullptr;
}

static QGroupBox *CtdGroup(const QString &title, const QString &text, QWidget *parent, QVBoxLayout **layoutOut)
{
	auto *group = new QGroupBox(title, parent);
	auto *layout = new QVBoxLayout(group);
	if (!text.isEmpty()) {
		auto *label = new QLabel(text, group);
		label->setWordWrap(true);
		layout->addWidget(label);
	}
	*layoutOut = layout;
	return group;
}

void OBSBasicSettings::CtdInitPage()
{
	auto *scroll = new QScrollArea(ui->settingsPages);
	scroll->setObjectName(QStringLiteral("ctdSettingsPage"));
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	auto *body = new QWidget(scroll);
	auto *layout = new QVBoxLayout(body);
	layout->setContentsMargins(0, 0, 0, 0);

	/* CTD Relay and CTD Hub: the CTD plugin's own section */
	QWidget *section = nullptr;
	{
		calldata_t cd = {};
		calldata_set_ptr(&cd, "parent", body);
		if (proc_handler_call(obs_get_proc_handler(), "ctd_settings_widget", &cd)) {
			section = static_cast<QWidget *>(calldata_ptr(&cd, "widget"));
		}
		calldata_free(&cd);
	}
	if (section) {
		layout->addWidget(section);
	} else if (main && main->findChild<QDockWidget *>(QString::fromLatin1(kCtdRelayDockId))) {
		QVBoxLayout *relayLayout;
		layout->addWidget(CtdGroup(QTStr("CTD.Relay"), QTStr("CTD.Settings.RelayInDock"), body, &relayLayout));
	}

	/* Vertical: its own settings window */
	if (CtdVerticalSettingsButton(main)) {
		QVBoxLayout *verticalLayout;
		layout->addWidget(CtdGroup(QTStr("CTD.Settings.Vertical"), QTStr("CTD.Settings.Vertical.Text"), body,
					   &verticalLayout));
		auto *open = new QPushButton(QTStr("CTD.Settings.Vertical.Open"), body);
		verticalLayout->addWidget(open, 0, Qt::AlignLeft);
		connect(open, &QPushButton::clicked, this, [this]() {
			if (QPushButton *button = CtdVerticalSettingsButton(main)) {
				button->click();
			}
		});
	}

	/* Advanced (gap 19): OBS's own tools, which open as separate windows once Settings closes */
	QVBoxLayout *advancedLayout;
	layout->addWidget(
		CtdGroup(QTStr("CTD.Settings.Advanced"), QTStr("CTD.Settings.Advanced.Text"), body, &advancedLayout));
	auto *row = new QHBoxLayout();
	auto addTool = [&](QAction *action) {
		if (!action) {
			return;
		}
		auto *button = new QPushButton(action->text() + QStringLiteral("…"), body);
		row->addWidget(button);
		QPointer<QAction> target = action;
		connect(button, &QPushButton::clicked, this, [this, target]() {
			/* close() asks about unsaved changes first; open the tool only if Settings really closed */
			if (close() && target) {
				QMetaObject::invokeMethod(target.data(), &QAction::trigger, Qt::QueuedConnection);
			}
		});
	};
	if (main) {
		addTool(main->ui->actionOpenPluginManager);
		addTool(CtdFindModuleToolsAction(main->ui->menuTools, "frontend-tools", "Scripts"));
		addTool(CtdFindModuleToolsAction(main->ui->menuTools, "obs-websocket", "OBSWebSocket.Settings.DialogTitle"));
	}
	row->addStretch(1);
	advancedLayout->addLayout(row);

	layout->addStretch(1);
	scroll->setWidget(body);
	ui->settingsPages->addWidget(scroll);

	auto *item = new QListWidgetItem(QIcon(QStringLiteral(":/res/images/obs.png")), QTStr("CTD.Settings"),
					 ui->listWidget);
	ctdPageRow = ui->listWidget->row(item);
}

void OBSBasicSettings::CtdShowPage()
{
	if (ctdPageRow >= 0) {
		ui->listWidget->setCurrentRow(ctdPageRow);
	}
}
