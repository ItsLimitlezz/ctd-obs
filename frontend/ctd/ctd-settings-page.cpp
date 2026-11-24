/******************************************************************************
    CTD OBS: the Settings ▸ CTD page (integration gaps 7 and 19).

      CTD Relay             the CTD plugin's section (connection, pairing,
                            profile, WebSocket sharing, feed servers)
      Vertical              the vertical canvas's own settings window
      Updates               check for updates automatically, update channel
                            (frontend/ctd/ctd-update.cpp; saved on OK/Apply)
      NDI                   Off / Send, the NDI outputs (program, vertical,
                            each NDI Output filter: rename, stop), Set up
                            from the NDI Runtime (plugins/ctd-ndi,
                            docs/ctd/ndi.md; saved and applied on OK/Apply)
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

#include "ctd-ndi.hpp"
#include "ctd-update-logic.hpp"

#include "settings/OBSBasicSettings.hpp"
#include "ui_OBSBasicSettings.h"

#include <widgets/OBSBasic.hpp>

#include <qt-wrappers.hpp>

#include <OBSApp.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cstring>

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

	/* CTD Relay (with its incoming feeds' servers): the CTD plugin's own section */
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

	/* Updates: CTD OBS's own update check (Windows and macOS builds) */
	if (CtdCurrentPlatform() != CtdUpdatePlatform::Other) {
		QVBoxLayout *updatesLayout;
		layout->addWidget(CtdGroup(QTStr("CTD.Settings.Updates"), QTStr("CTD.Settings.Updates.Text"), body,
					   &updatesLayout));
		config_t *config = App()->GetUserConfig();

		ctdUpdateAuto = new QCheckBox(QTStr("CTD.Settings.Updates.Auto"), body);
		ctdUpdateAuto->setChecked(config_get_bool(config, kCtdUpdateSection, kCtdUpdateKeyAuto));
		updatesLayout->addWidget(ctdUpdateAuto);

		auto *form = new QFormLayout();
		ctdUpdateChannel = new QComboBox(body);
		ctdUpdateChannel->addItem(QTStr("CTD.Settings.Updates.Channel.Stable"), QStringLiteral("stable"));
		ctdUpdateChannel->addItem(QTStr("CTD.Settings.Updates.Channel.Test"), QStringLiteral("test"));
		const char *channel = config_get_string(config, kCtdUpdateSection, kCtdUpdateKeyChannel);
		ctdUpdateChannel->setCurrentIndex(channel && strcmp(channel, "test") == 0 ? 1 : 0);
		form->addRow(QTStr("CTD.Settings.Updates.Channel"), ctdUpdateChannel);
		updatesLayout->addLayout(form);

		auto changed = [this]() {
			ctdChanged = true;
			EnableApplyButton(true);
		};
		connect(ctdUpdateAuto, &QCheckBox::toggled, this, changed);
		connect(ctdUpdateChannel, &QComboBox::currentIndexChanged, this, changed);
	}

	/* NDI (docs/ctd/ndi.md) */
	CtdInitNdiSection(body, layout);

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

void OBSBasicSettings::CtdSaveSettings()
{
	config_t *config = App()->GetUserConfig();
	if (ctdUpdateAuto) {
		config_set_bool(config, kCtdUpdateSection, kCtdUpdateKeyAuto, ctdUpdateAuto->isChecked());
	}
	if (ctdUpdateChannel) {
		config_set_string(config, kCtdUpdateSection, kCtdUpdateKeyChannel,
				  QT_TO_UTF8(ctdUpdateChannel->currentData().toString()));
	}
	if (ctdNdiMode) {
		config_set_string(config, kCtdNdiSection, kCtdNdiKeyMode,
				  QT_TO_UTF8(ctdNdiMode->currentData().toString()));
		CtdNdiSaveOutputs();
		blog(LOG_INFO, "[ctd-ndi] Settings: NDI %s, send program %s, send vertical %s",
		     QT_TO_UTF8(ctdNdiMode->currentData().toString()),
		     config_get_bool(config, kCtdNdiSection, kCtdNdiKeySendProgram) ? "on" : "off",
		     config_get_bool(config, kCtdNdiSection, kCtdNdiKeySendVertical) ? "on" : "off");
		/* takes effect at once: going Off stops every sender */
		CtdNdiApply();
		CtdNdiFillOutputs();
		CtdNdiRefreshStatus();
	}
}

/* NDI: on / off, the outputs list (program, vertical, every NDI Output filter), the library's state. Without the NDI
 * module only a note. */
void OBSBasicSettings::CtdInitNdiSection(QWidget *body, QVBoxLayout *layout)
{
	auto *group = new QGroupBox(QTStr("CTD.Settings.NDI"), body);
	auto *groupLayout = new QVBoxLayout(group);
	layout->addWidget(group);

	auto *intro = new QLabel(QTStr("CTD.Settings.NDI.Text"), group);
	intro->setWordWrap(true);
	intro->setTextFormat(Qt::RichText);
	intro->setOpenExternalLinks(true);
	groupLayout->addWidget(intro);

	const CtdNdiStatus status = CtdNdiQueryStatus();
	if (!status.available) {
		auto *none = new QLabel(QTStr("CTD.NDI.NoModule"), group);
		none->setWordWrap(true);
		groupLayout->addWidget(none);
		return;
	}

	CtdNdiSetConfigDefaults();
	config_t *config = App()->GetUserConfig();

	auto *form = new QFormLayout();
	ctdNdiMode = new QComboBox(group);
	ctdNdiMode->addItem(QTStr("CTD.Settings.NDI.Mode.Off"), QStringLiteral("off"));
	ctdNdiMode->addItem(QTStr("CTD.Settings.NDI.Mode.Send"), QStringLiteral("send"));
	const char *mode = config_get_string(config, kCtdNdiSection, kCtdNdiKeyMode);
	const int index = ctdNdiMode->findData(QString::fromUtf8(mode ? mode : "off"));
	ctdNdiMode->setCurrentIndex(index >= 0 ? index : 0);
	form->addRow(QTStr("CTD.Settings.NDI.Mode"), ctdNdiMode);
	groupLayout->addLayout(form);

	/* the outputs: program and vertical (settings), then each NDI Output filter (saved with the scene collection) */
	auto *outputsLabel = new QLabel(QTStr("CTD.Settings.NDI.Outputs"), group);
	outputsLabel->setWordWrap(true);
	groupLayout->addWidget(outputsLabel);
	ctdNdiOutputs = new QTreeWidget(group);
	ctdNdiOutputs->setColumnCount(3);
	ctdNdiOutputs->setHeaderLabels({QTStr("CTD.Settings.NDI.Outputs.Sender"),
					QTStr("CTD.Settings.NDI.Outputs.Source"),
					QTStr("CTD.Settings.NDI.Outputs.State")});
	ctdNdiOutputs->setRootIsDecorated(false);
	ctdNdiOutputs->setUniformRowHeights(true);
	ctdNdiOutputs->setMinimumHeight(160);
	groupLayout->addWidget(ctdNdiOutputs);

	auto *outputsRow = new QHBoxLayout();
	auto *refresh = new QPushButton(QTStr("CTD.Settings.NDI.Outputs.Refresh"), group);
	outputsRow->addWidget(refresh);
	outputsRow->addStretch(1);
	groupLayout->addLayout(outputsRow);

	ctdNdiStatus = new QLabel(group);
	ctdNdiStatus->setWordWrap(true);
	groupLayout->addWidget(ctdNdiStatus);

	auto *row = new QHBoxLayout();
	auto *setup = new QPushButton(QTStr("CTD.Settings.NDI.Setup"), group);
	row->addWidget(setup);
	auto *runtime = new QLabel(QTStr("CTD.Settings.NDI.Runtime")
#ifdef __APPLE__
					   .arg(QStringLiteral("https://ndi.link/NDIRedistV6Apple")),
#else
					   .arg(QStringLiteral("https://ndi.link/NDIRedistV6")),
#endif
				   group);
	runtime->setTextFormat(Qt::RichText);
	runtime->setOpenExternalLinks(true);
	row->addWidget(runtime);
	row->addStretch(1);
	groupLayout->addLayout(row);

	ctdNdiWarning = new QLabel(group);
	ctdNdiWarning->setWordWrap(true);
	ctdNdiWarning->setProperty("class", "text-warning");
	groupLayout->addWidget(ctdNdiWarning);

	CtdNdiFillOutputs();

	auto changed = [this]() {
		ctdChanged = true;
		EnableApplyButton(true);
	};
	connect(ctdNdiMode, &QComboBox::currentIndexChanged, this, changed);
	connect(ctdNdiOutputs, &QTreeWidget::itemChanged, this, changed);
	connect(refresh, &QPushButton::clicked, this, [this]() {
		CtdNdiFillOutputs();
		CtdNdiRefreshStatus();
	});
	connect(setup, &QPushButton::clicked, this, [this]() {
		std::string message;
		const bool ok = CtdNdiSetup(message);
		CtdNdiRefreshStatus();
		if (!ok) {
			OBSMessageBox::warning(this, QTStr("CTD.Settings.NDI"), QString::fromStdString(message));
		}
	});

	CtdNdiRefreshStatus();
}

/* Row data: the kind of row and, for program / vertical, its user.ini key */
enum CtdNdiRowKind { CtdNdiRowProgram = 1, CtdNdiRowVertical = 2, CtdNdiRowFilter = 3 };
static constexpr int kCtdNdiKindRole = Qt::UserRole;
static constexpr int kCtdNdiIndexRole = Qt::UserRole + 1;
static constexpr int kCtdNdiNameRole = Qt::UserRole + 2; /* the name as loaded, to see whether it was edited */

static std::string CtdNdiFilterState(obs_source_t *filter)
{
	std::string state;
	calldata_t cd = {};
	calldata_set_ptr(&cd, "filter", filter);
	if (proc_handler_call(obs_get_proc_handler(), "ctd_ndi_filter_state", &cd)) {
		const char *text = calldata_string(&cd, "state");
		state = text ? text : "";
	}
	calldata_free(&cd);
	return state;
}

void OBSBasicSettings::CtdNdiFillOutputs()
{
	if (!ctdNdiOutputs) {
		return;
	}
	const QSignalBlocker blocker(ctdNdiOutputs);
	ctdNdiOutputs->clear();
	ctdNdiFilterRows.clear();

	const CtdNdiStatus status = CtdNdiQueryStatus();
	config_t *config = App()->GetUserConfig();
	auto sending = [](bool on) {
		return on ? QTStr("CTD.Settings.NDI.State.Sending") : QTStr("CTD.Settings.NDI.State.NotSending");
	};

	auto *program = new QTreeWidgetItem(ctdNdiOutputs,
					    {QStringLiteral("CTD OBS Program"),
					     QTStr("CTD.Settings.NDI.Outputs.Program"), sending(status.program)});
	program->setData(0, kCtdNdiKindRole, CtdNdiRowProgram);
	program->setFlags((program->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
	program->setCheckState(0, config_get_bool(config, kCtdNdiSection, kCtdNdiKeySendProgram) ? Qt::Checked
												 : Qt::Unchecked);

	auto *vertical = new QTreeWidgetItem(ctdNdiOutputs,
					     {QStringLiteral("CTD OBS Vertical"),
					      QTStr("CTD.Settings.NDI.Outputs.Vertical"), sending(status.vertical)});
	vertical->setData(0, kCtdNdiKindRole, CtdNdiRowVertical);
	vertical->setFlags((vertical->flags() | Qt::ItemIsUserCheckable) & ~Qt::ItemIsEditable);
	vertical->setCheckState(0, config_get_bool(config, kCtdNdiSection, kCtdNdiKeySendVertical) ? Qt::Checked
												   : Qt::Unchecked);

	/* every NDI Output filter on an input, a scene or a group */
	struct Found {
		std::vector<std::pair<OBSSource, OBSSource>> filters; /* parent, filter */
	} found;
	auto collect = [](void *param, obs_source_t *parent) {
		auto *f = static_cast<Found *>(param);
		obs_source_enum_filters(
			parent,
			[](obs_source_t *p, obs_source_t *filter, void *param2) {
				const char *id = obs_source_get_id(filter);
				if (id && strcmp(id, "ctd_ndi_filter") == 0) {
					static_cast<Found *>(param2)->filters.emplace_back(OBSSource(p),
											   OBSSource(filter));
				}
			},
			f);
		return true;
	};
	obs_enum_sources(collect, &found);
	obs_enum_scenes(collect, &found);

	for (auto &[parent, filter] : found.filters) {
		OBSDataAutoRelease settings = obs_source_get_settings(filter);
		const char *name = obs_data_get_string(settings, "ndi_name");
		const QString sourceName = QT_UTF8(obs_source_get_name(parent));
		const QString senderName = name && *name ? QT_UTF8(name) : sourceName;
		std::string state = CtdNdiFilterState(filter);
		auto *item =
			new QTreeWidgetItem(ctdNdiOutputs, {senderName, sourceName, QString::fromStdString(state)});
		item->setData(0, kCtdNdiKindRole, CtdNdiRowFilter);
		item->setData(0, kCtdNdiIndexRole, static_cast<int>(ctdNdiFilterRows.size()));
		item->setData(0, kCtdNdiNameRole, senderName);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
		item->setCheckState(0, obs_source_enabled(filter) ? Qt::Checked : Qt::Unchecked);
		item->setToolTip(0, QTStr("CTD.Settings.NDI.Outputs.EditHint"));
		ctdNdiFilterRows.push_back(OBSGetWeakRef(filter));
	}
	for (int column = 0; column < 3; column++) {
		ctdNdiOutputs->resizeColumnToContents(column);
	}
}

void OBSBasicSettings::CtdNdiSaveOutputs()
{
	if (!ctdNdiOutputs) {
		return;
	}
	config_t *config = App()->GetUserConfig();
	for (int i = 0; i < ctdNdiOutputs->topLevelItemCount(); i++) {
		QTreeWidgetItem *item = ctdNdiOutputs->topLevelItem(i);
		const bool on = item->checkState(0) == Qt::Checked;
		switch (item->data(0, kCtdNdiKindRole).toInt()) {
		case CtdNdiRowProgram:
			config_set_bool(config, kCtdNdiSection, kCtdNdiKeySendProgram, on);
			break;
		case CtdNdiRowVertical:
			config_set_bool(config, kCtdNdiSection, kCtdNdiKeySendVertical, on);
			break;
		case CtdNdiRowFilter: {
			const int index = item->data(0, kCtdNdiIndexRole).toInt();
			if (index < 0 || index >= static_cast<int>(ctdNdiFilterRows.size())) {
				break;
			}
			OBSSourceAutoRelease filter = obs_weak_source_get_source(ctdNdiFilterRows[index]);
			if (!filter) {
				break; /* removed while Settings was open */
			}
			/* stop / start: the filter's own enabled state, saved with the scene collection */
			if (obs_source_enabled(filter) != on) {
				obs_source_set_enabled(filter, on);
			}
			const QString name = item->text(0).trimmed();
			if (name != item->data(0, kCtdNdiNameRole).toString()) {
				OBSDataAutoRelease settings = obs_source_get_settings(filter);
				obs_data_set_string(settings, "ndi_name", QT_TO_UTF8(name));
				obs_source_update(filter, settings);
			}
			break;
		}
		default:
			break;
		}
	}
}

void OBSBasicSettings::CtdNdiRefreshStatus()
{
	if (!ctdNdiStatus) {
		return;
	}
	const CtdNdiStatus status = CtdNdiQueryStatus();

	QStringList lines;
	if (status.launchDisabled) {
		lines << QTStr("CTD.Settings.NDI.LaunchOff");
	}
	if (status.loaded) {
		lines << QTStr("CTD.Settings.NDI.Loaded").arg(QString::fromStdString(status.version));
		if (status.mode == "off") {
			lines << QTStr("CTD.Settings.NDI.OffUntilRestart");
		} else if (status.paused) {
			lines << QTStr("CTD.Settings.NDI.Paused");
		} else {
			lines << QTStr("CTD.Settings.NDI.Active").arg(status.senders).arg(status.maxSenders);
		}
	} else if (!status.installed.empty()) {
		lines << QTStr("CTD.Settings.NDI.Installed").arg(QString::fromStdString(status.installed));
	} else {
		lines << QTStr("CTD.Settings.NDI.NotSetUp");
	}
	if (!status.loaded && !status.message.empty()) {
		lines << QString::fromStdString(status.message);
	}
	ctdNdiStatus->setText(lines.join(QStringLiteral("\n")));

	if (!status.systemPieces.empty()) {
		ctdNdiWarning->setText(
			QTStr("CTD.Settings.NDI.SystemPieces").arg(QString::fromStdString(status.systemPieces)));
		ctdNdiWarning->show();
	} else {
		ctdNdiWarning->hide();
	}
}

void OBSBasicSettings::CtdShowPage()
{
	if (ctdPageRow >= 0) {
		ui->listWidget->setCurrentRow(ctdPageRow);
	}
}
