/******************************************************************************
    CTD OBS: the memory watchdog (docs/ctd/ndi.md, "Memory watchdog").

    Every 10 s it reads the process footprint (macOS phys_footprint, which
    includes IOSurfaces and graphics memory; Windows private bytes plus the
    process's local video memory). Every 60 s it logs the numbers with NDI's
    state. On sustained growth (ctd-memory-watchdog-logic.cpp) or above
    [CTD] WatchdogLimitMB it shows a non-blocking warning, naming NDI when
    it is active and offering to pause it.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include "ctd-memory-watchdog-logic.hpp"

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>

#include <cstdint>

class QMessageBox;
class QTimer;
class QWidget;

/* The footprint as the watchdog counts it; 0 if it can't be read */
uint64_t CtdProcessFootprintBytes();

class CtdMemoryWatchdog : public QObject {
public:
	explicit CtdMemoryWatchdog(QWidget *mainWindow);

private:
	void Sample();
	void Warn(CtdWatchdogWarning warning, uint64_t bytes, double rate);

	QWidget *mainWindow;
	QTimer *timer;
	QElapsedTimer clock;
	CtdWatchdogLogic logic;
	QPointer<QMessageBox> box;
};
