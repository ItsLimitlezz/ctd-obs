/******************************************************************************
    Copyright (C) 2023 by Lain Bailey <lain@obsproject.com>
                          Zachary Lund <admin@computerquip.com>
                          Philippe Groarke <philippe.groarke@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
******************************************************************************/

#include "OBSBasic.hpp"

#include <qt-wrappers.hpp>

#include <algorithm>
#include <utility>

void setupDockAction(QDockWidget *dock)
{
	QAction *action = dock->toggleViewAction();

	auto neverDisable = [action]() {
		QSignalBlocker block(action);
		action->setEnabled(true);
	};

	auto newToggleView = [dock](bool check) {
		QSignalBlocker block(dock);
		dock->setVisible(check);
	};

	// Replace the slot connected by default
	QObject::disconnect(action, &QAction::triggered, nullptr, 0);
	QObject::connect(action, &QAction::triggered, dock, newToggleView);

	// Make the action unable to be disabled
	QObject::connect(action, &QAction::enabledChanged, action, neverDisable);
}

/* CTD OBS: dock ids of CTD OBS's built-in modules (CTD plugin, vertical canvas). They're part of the app: Reset UI
 * doesn't warn about them and docks them like OBS's own. These are the modules' saved dock ids; never rename them. */
static const char *const kCtdRelayDock = "ctd-multistream";
static const char *const kCtdHubDock = "ctd-hub";
static const char *const kCtdVerticalDock = "VerticalCanvasDock";
static const char *const kCtdVerticalScenesDock = "VerticalCanvasDockScenes";
static const char *const kCtdVerticalSourcesDock = "VerticalCanvasDockSources";
static const char *const kCtdVerticalTransitionsDock = "VerticalCanvasDockTransitions";

bool OBSBasic::IsCtdBuiltInDock(const QString &name)
{
	for (const char *id : {kCtdRelayDock, kCtdHubDock, kCtdVerticalDock, kCtdVerticalScenesDock,
			       kCtdVerticalSourcesDock, kCtdVerticalTransitionsDock}) {
		if (name == QLatin1String(id)) {
			return true;
		}
	}
	return false;
}

QDockWidget *OBSBasic::FindExtraDock(const char *name) const
{
	for (const std::shared_ptr<QDockWidget> &dock : extraDocks) {
		if (dock && dock->objectName() == QLatin1String(name)) {
			return dock.get();
		}
	}
	return nullptr;
}

/* CTD OBS: the vertical canvas on the right with its Scenes / Sources / Transitions docks under OBS's own (where the
 * vertical canvas puts them on its first run), CTD Relay docked right of it and visible, CTD Hub tabbed behind Relay */
void OBSBasic::ResetCtdDocks()
{
	QDockWidget *vertical = FindExtraDock(kCtdVerticalDock);
	QDockWidget *relay = FindExtraDock(kCtdRelayDock);
	QDockWidget *hub = FindExtraDock(kCtdHubDock);

	if (vertical) {
		vertical->setFloating(false);
		addDockWidget(Qt::RightDockWidgetArea, vertical);
		vertical->setVisible(true);

		const std::pair<const char *, QDockWidget *> companions[] = {
			{kCtdVerticalScenesDock, ui->scenesDock},
			{kCtdVerticalSourcesDock, ui->sourcesDock},
			{kCtdVerticalTransitionsDock, ui->transitionsDock},
		};
		for (const auto &[id, beside] : companions) {
			QDockWidget *dock = FindExtraDock(id);
			if (!dock) {
				continue;
			}
			dock->setFloating(false);
			const Qt::DockWidgetArea area = beside ? dockWidgetArea(beside) : Qt::NoDockWidgetArea;
			if (area == Qt::NoDockWidgetArea || beside->isFloating()) {
				addDockWidget(Qt::RightDockWidgetArea, dock);
				splitDockWidget(vertical, dock, Qt::Horizontal);
			} else {
				addDockWidget(area, dock);
				splitDockWidget(beside, dock, Qt::Vertical);
			}
			dock->setVisible(true);
		}
	}

	if (relay) {
		relay->setFloating(false);
		addDockWidget(Qt::RightDockWidgetArea, relay);
		if (vertical) {
			splitDockWidget(vertical, relay, Qt::Horizontal);
		}
		relay->setVisible(true);
	}

	if (hub) {
		hub->setFloating(false);
		addDockWidget(Qt::RightDockWidgetArea, hub);
		if (relay) {
			tabifyDockWidget(relay, hub);
		}
		hub->setVisible(true);
	}

	if (relay) {
		relay->raise();
		resizeDocks({relay}, {std::min(width() * 25 / 100, 340)}, Qt::Horizontal);
	}
}

void OBSBasic::on_resetDocks_triggered(bool force)
{
	/* CTD OBS: only docks from outside CTD OBS (other plugins, custom browser docks) are worth the warning */
	const bool thirdPartyDocks = std::any_of(extraDocks.begin(), extraDocks.end(),
						 [](const std::shared_ptr<QDockWidget> &dock) {
							 return dock && !IsCtdBuiltInDock(dock->objectName());
						 });
#ifdef BROWSER_AVAILABLE
	if ((thirdPartyDocks || extraCustomDocks.size() || extraBrowserDocks.size()) && !force)
#else
	if ((thirdPartyDocks || extraCustomDocks.size()) && !force)
#endif
	{
		QMessageBox::StandardButton button =
			OBSMessageBox::question(this, QTStr("ResetUIWarning.Title"), QTStr("ResetUIWarning.Text"));

		if (button == QMessageBox::No) {
			return;
		}
	}

#define RESET_DOCKLIST(dockList)                                                                               \
	for (int i = dockList.size() - 1; i >= 0; i--) {                                                       \
		dockList[i]->setVisible(true);                                                                 \
		dockList[i]->setFloating(true);                                                                \
		dockList[i]->move(frameGeometry().topLeft() + rect().center() - dockList[i]->rect().center()); \
		dockList[i]->setVisible(false);                                                                \
	}

	RESET_DOCKLIST(extraDocks)
	RESET_DOCKLIST(extraCustomDocks)
#ifdef BROWSER_AVAILABLE
	RESET_DOCKLIST(extraBrowserDocks)
#endif
#undef RESET_DOCKLIST

	restoreState(startingDockLayout);
	ui->sideDocks->setChecked(true);

	int cx = width();
	int bottomDocksHeight = height();

	bottomDocksHeight = bottomDocksHeight * 225 / 1000;

	ui->scenesDock->setVisible(true);
	ui->sourcesDock->setVisible(true);
	ui->mixerDock->setVisible(true);
	ui->transitionsDock->setVisible(true);
	controlsDock->setVisible(true);
	statsDock->setVisible(false);
	statsDock->setFloating(true);

	QList<QDockWidget *> bottomDocks{ui->mixerDock, ui->transitionsDock, controlsDock};

	resizeDocks(bottomDocks, {bottomDocksHeight, bottomDocksHeight, bottomDocksHeight}, Qt::Vertical);
	resizeDocks(bottomDocks, {cx * 45 / 100, cx * 14 / 100, cx * 16 / 100}, Qt::Horizontal);

	int sideDockWidth = std::min(width() * 30 / 100, 280);
	resizeDocks({ui->scenesDock, ui->sourcesDock}, {sideDockWidth, sideDockWidth}, Qt::Horizontal);

	ResetCtdDocks();

	activateWindow();
}

void OBSBasic::on_lockDocks_toggled(bool lock)
{
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	QDockWidget::DockWidgetFeatures mainFeatures = features;
	mainFeatures &= ~QDockWidget::QDockWidget::DockWidgetClosable;

	ui->scenesDock->setFeatures(mainFeatures);
	ui->sourcesDock->setFeatures(mainFeatures);
	ui->mixerDock->setFeatures(mainFeatures);
	ui->transitionsDock->setFeatures(mainFeatures);
	controlsDock->setFeatures(mainFeatures);
	statsDock->setFeatures(features);

	for (int i = extraDocks.size() - 1; i >= 0; i--) {
		extraDocks[i]->setFeatures(features);
	}

	for (int i = extraCustomDocks.size() - 1; i >= 0; i--) {
		extraCustomDocks[i]->setFeatures(features);
	}

#ifdef BROWSER_AVAILABLE
	for (int i = extraBrowserDocks.size() - 1; i >= 0; i--) {
		extraBrowserDocks[i]->setFeatures(features);
	}
#endif
}

void OBSBasic::on_sideDocks_toggled(bool side)
{
	config_set_bool(App()->GetUserConfig(), "BasicWindow", "SideDocks", side);

	setDockCornersVertical(side);
}

void OBSBasic::AddDockWidget(QDockWidget *dock, Qt::DockWidgetArea area, bool extraBrowser)
{
	if (dock->objectName().isEmpty()) {
		return;
	}

	bool lock = ui->lockDocks->isChecked();
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	setupDockAction(dock);
	dock->setFeatures(features);
	addDockWidget(area, dock);

#ifdef BROWSER_AVAILABLE
	if (extraBrowser && extraBrowserMenuDocksSeparator.isNull()) {
		extraBrowserMenuDocksSeparator = ui->menuDocks->addSeparator();
	}

	if (!extraBrowser && !extraBrowserMenuDocksSeparator.isNull()) {
		ui->menuDocks->insertAction(extraBrowserMenuDocksSeparator, dock->toggleViewAction());
	} else {
		ui->menuDocks->addAction(dock->toggleViewAction());
	}

	if (extraBrowser) {
		return;
	}
#else
	UNUSED_PARAMETER(extraBrowser);

	ui->menuDocks->addAction(dock->toggleViewAction());
#endif

	extraDockNames.push_back(dock->objectName());
	extraDocks.push_back(std::shared_ptr<QDockWidget>(dock));
}

void OBSBasic::RemoveDockWidget(const QString &name)
{
	if (extraDockNames.contains(name)) {
		int idx = extraDockNames.indexOf(name);
		extraDockNames.removeAt(idx);
		extraDocks[idx].reset();
		extraDocks.removeAt(idx);
	} else if (extraCustomDockNames.contains(name)) {
		int idx = extraCustomDockNames.indexOf(name);
		extraCustomDockNames.removeAt(idx);
		removeDockWidget(extraCustomDocks[idx]);
		extraCustomDocks.removeAt(idx);
	}
}

bool OBSBasic::IsDockObjectNameUsed(const QString &name)
{
	QStringList list;
	list << "scenesDock"
	     << "sourcesDock"
	     << "mixerDock"
	     << "transitionsDock"
	     << "controlsDock"
	     << "statsDock";
	list << extraDockNames;
	list << extraCustomDockNames;

	return list.contains(name);
}

void OBSBasic::AddCustomDockWidget(QDockWidget *dock)
{
	// Prevent the object name from being changed
	connect(dock, &QObject::objectNameChanged, this, &OBSBasic::RepairCustomExtraDockName);

	bool lock = ui->lockDocks->isChecked();
	QDockWidget::DockWidgetFeatures features =
		lock ? QDockWidget::NoDockWidgetFeatures
		     : (QDockWidget::DockWidgetClosable | QDockWidget::DockWidgetMovable |
			QDockWidget::DockWidgetFloatable);

	dock->setFeatures(features);
	addDockWidget(Qt::RightDockWidgetArea, dock);

	extraCustomDockNames.push_back(dock->objectName());
	extraCustomDocks.push_back(dock);
}

void OBSBasic::setDockCornersVertical(bool vertical)
{
	if (vertical) {
		setCorner(Qt::TopLeftCorner, Qt::LeftDockWidgetArea);
		setCorner(Qt::TopRightCorner, Qt::RightDockWidgetArea);
		setCorner(Qt::BottomLeftCorner, Qt::LeftDockWidgetArea);
		setCorner(Qt::BottomRightCorner, Qt::RightDockWidgetArea);
	} else {
		setCorner(Qt::TopLeftCorner, Qt::TopDockWidgetArea);
		setCorner(Qt::TopRightCorner, Qt::TopDockWidgetArea);
		setCorner(Qt::BottomLeftCorner, Qt::BottomDockWidgetArea);
		setCorner(Qt::BottomRightCorner, Qt::BottomDockWidgetArea);
	}
}

void OBSBasic::RepairCustomExtraDockName()
{
	QDockWidget *dock = reinterpret_cast<QDockWidget *>(sender());
	int idx = extraCustomDocks.indexOf(dock);
	QSignalBlocker block(dock);

	if (idx == -1) {
		blog(LOG_WARNING, "A custom dock got its object name changed");
		return;
	}

	blog(LOG_WARNING, "The custom dock '%s' got its object name restored", QT_TO_UTF8(extraCustomDockNames[idx]));

	dock->setObjectName(extraCustomDockNames[idx]);
}
