/******************************************************************************
    CTD OBS: the memory watchdog (ctd-memory-watchdog.hpp, docs/ctd/ndi.md).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ctd-memory-watchdog.hpp"
#include "ctd-ndi.hpp"

#include <OBSApp.hpp>
#include <qt-wrappers.hpp>

#include <QMessageBox>
#include <QPushButton>
#include <QTimer>
#include <QWidget>

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#include <dxgi1_4.h>
#else
#include <cstdio>
#include <unistd.h>
#endif

#include <cmath>

static constexpr int kSampleMs = 10 * 1000;

#ifdef _WIN32
/* The process's local video memory on the adapter OBS renders with (Video ▸ AdapterIdx) */
static uint64_t CtdVideoMemoryBytes()
{
	static IDXGIAdapter3 *adapter = nullptr;
	static bool tried = false;
	if (!tried) {
		tried = true;
		IDXGIFactory1 *factory = nullptr;
		if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), reinterpret_cast<void **>(&factory)))) {
			const UINT index =
				static_cast<UINT>(config_get_uint(App()->GetUserConfig(), "Video", "AdapterIdx"));
			IDXGIAdapter1 *adapter1 = nullptr;
			if (SUCCEEDED(factory->EnumAdapters1(index, &adapter1)) ||
			    SUCCEEDED(factory->EnumAdapters1(0, &adapter1))) {
				adapter1->QueryInterface(__uuidof(IDXGIAdapter3), reinterpret_cast<void **>(&adapter));
				adapter1->Release();
			}
			factory->Release();
		}
	}
	if (!adapter) {
		return 0;
	}
	DXGI_QUERY_VIDEO_MEMORY_INFO info = {};
	if (FAILED(adapter->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
		return 0;
	}
	return info.CurrentUsage;
}
#endif

uint64_t CtdProcessFootprintBytes()
{
#if defined(__APPLE__)
	task_vm_info_data_t info = {};
	mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
	if (task_info(mach_task_self(), TASK_VM_INFO, reinterpret_cast<task_info_t>(&info), &count) != KERN_SUCCESS) {
		return 0;
	}
	return info.phys_footprint;
#elif defined(_WIN32)
	PROCESS_MEMORY_COUNTERS_EX counters = {};
	counters.cb = sizeof(counters);
	if (!GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&counters),
				  sizeof(counters))) {
		return 0;
	}
	return static_cast<uint64_t>(counters.PrivateUsage) + CtdVideoMemoryBytes();
#else
	long pages = 0, resident = 0;
	FILE *f = fopen("/proc/self/statm", "r");
	if (!f) {
		return 0;
	}
	if (fscanf(f, "%ld %ld", &pages, &resident) != 2) {
		resident = 0;
	}
	fclose(f);
	return static_cast<uint64_t>(resident) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#endif
}

static CtdWatchdogConfig ConfigFromUser()
{
	CtdWatchdogConfig config;
	config_t *user = App()->GetUserConfig();
	if (user) {
		const uint64_t limit = config_get_uint(user, kCtdNdiSection, kCtdNdiKeyWatchdogLimit);
		if (limit >= 512) {
			config.limitMb = limit;
		}
	}
	return config;
}

CtdMemoryWatchdog::CtdMemoryWatchdog(QWidget *mainWindow_)
	: QObject(mainWindow_),
	  mainWindow(mainWindow_),
	  timer(new QTimer(this)),
	  logic(ConfigFromUser())
{
	clock.start();
	blog(LOG_INFO, "[ctd-watchdog] Watching memory: warn above %llu MB or on %.0f MB/min for %d min",
	     static_cast<unsigned long long>(logic.Config().limitMb), logic.Config().rateMbPerMin,
	     logic.Config().sustainMinutes);
	timer->setInterval(kSampleMs);
	connect(timer, &QTimer::timeout, this, [this]() { Sample(); });
	timer->start();
	Sample();
}

void CtdMemoryWatchdog::Sample()
{
	const uint64_t bytes = CtdProcessFootprintBytes();
	if (bytes == 0) {
		return;
	}
	logic.AddSample(static_cast<double>(clock.elapsed()) / 1000.0, bytes);

	const int windowMinutes = logic.Config().sustainMinutes;
	if (logic.LogDue()) {
		const CtdNdiStatus ndi = CtdNdiQueryStatus();
		CtdWatchdogNdiStatus status;
		status.mode = ndi.available ? ndi.mode : "off";
		status.paused = ndi.paused;
		status.senders = ndi.senders;
		status.droppedFrames = static_cast<uint64_t>(ndi.dropped < 0 ? 0 : ndi.dropped);
		const std::string line =
			CtdWatchdogLogLine(bytes, logic.GrowthMbPerMin(windowMinutes * 60.0), windowMinutes, status);
		blog(LOG_INFO, "%s", line.c_str());
	}

	const CtdWatchdogWarning warning = logic.Evaluate();
	if (warning != CtdWatchdogWarning::None) {
		Warn(warning, bytes, logic.GrowthMbPerMin(windowMinutes * 60.0));
	}
}

void CtdMemoryWatchdog::Warn(CtdWatchdogWarning warning, uint64_t bytes, double rate)
{
	const CtdNdiStatus ndi = CtdNdiQueryStatus();
	const bool ndiActive = ndi.available && ndi.mode != "off" && !ndi.paused && ndi.senders > 0;
	const qulonglong mb = static_cast<qulonglong>(bytes / kCtdMiB);

	blog(LOG_WARNING, "[ctd-watchdog] WARNING: %s: footprint %llu MB, %+.0f MB/min%s",
	     warning == CtdWatchdogWarning::Growth ? "sustained memory growth" : "memory above the limit", mb, rate,
	     ndiActive ? "; NDI is active" : "");

	QString text =
		warning == CtdWatchdogWarning::Growth
			? QTStr("CTD.Watchdog.Growth").arg(mb).arg(static_cast<long long>(std::llround(rate)))
			: QTStr("CTD.Watchdog.Limit").arg(mb).arg(static_cast<qulonglong>(logic.Config().limitMb));
	if (ndiActive) {
		text += "\n\n" + QTStr("CTD.Watchdog.NdiActive").arg(ndi.senders);
	}

	if (box) {
		box->setText(text);
		box->show();
		box->raise();
		return;
	}

	/* non-blocking: shown, not exec()'d; nothing stops while it's open */
	box = new QMessageBox(QMessageBox::Warning, QTStr("CTD.Watchdog.Title"), text, QMessageBox::NoButton,
			      mainWindow);
	box->setAttribute(Qt::WA_DeleteOnClose);
	box->setModal(false);
	box->setWindowModality(Qt::NonModal);
	if (ndiActive) {
		QPushButton *pause = box->addButton(QTStr("CTD.Watchdog.PauseNdi"), QMessageBox::AcceptRole);
		connect(pause, &QPushButton::clicked, this, []() {
			blog(LOG_WARNING, "[ctd-watchdog] NDI paused from the memory warning");
			CtdNdiSetPaused(true);
		});
	}
	box->addButton(QTStr("CTD.Watchdog.Dismiss"), QMessageBox::RejectRole);
	box->show();
}
