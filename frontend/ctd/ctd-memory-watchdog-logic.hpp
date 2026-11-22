/******************************************************************************
    CTD OBS: the memory watchdog's maths (docs/ctd/ndi.md, "Memory watchdog").
    No Qt or OBS dependency; test/ctd-ndi unit-tests it.

    Samples (time in seconds, process footprint in bytes) come in every 10 s.
    A warning is due when the footprint grew by at least rateMbPerMin in
    each of the last sustainMinutes one-minute intervals (sustained growth,
    so one spike doesn't count), or when it is above limitMb; at most one
    warning per warnIntervalSec while that lasts. A log line is due every
    logIntervalSec.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#pragma once

#include <cstdint>
#include <deque>
#include <optional>
#include <string>

struct CtdWatchdogConfig {
	double rateMbPerMin = 200.0;
	int sustainMinutes = 3;
	uint64_t limitMb = 6144;
	double warnIntervalSec = 600.0;
	double logIntervalSec = 60.0;
};

enum class CtdWatchdogWarning { None, Growth, Limit };

/* What the NDI module reports (proc ctd_ndi_status); mode "off" when it isn't loaded */
struct CtdWatchdogNdiStatus {
	std::string mode = "off";
	bool paused = false;
	int receivers = 0;
	int sends = 0;
	uint64_t droppedFrames = 0;

	bool Active() const { return mode != "off" && !paused && (receivers > 0 || sends > 0); }
};

constexpr uint64_t kCtdMiB = 1024ull * 1024ull;

class CtdWatchdogLogic {
public:
	explicit CtdWatchdogLogic(CtdWatchdogConfig config_ = CtdWatchdogConfig()) : config(config_) {}

	/* Samples must come in time order; an older or equal time is ignored */
	void AddSample(double tSec, uint64_t bytes);

	/* The newest sample at or before t, if there is one */
	std::optional<uint64_t> BytesAt(double tSec) const;
	std::optional<uint64_t> LatestBytes() const;
	double LatestTime() const;

	/* Growth in MiB per minute over the last windowSec (or the history there is, if shorter); 0 with under 60 s */
	double GrowthMbPerMin(double windowSec) const;

	/* Each of the last sustainMinutes one-minute intervals grew by at least rateMbPerMin */
	bool SustainedGrowth() const;
	bool OverLimit() const;

	/* Whether to warn now (and remembers it, for the warning interval) */
	CtdWatchdogWarning Evaluate();
	/* Whether a log line is due now (and remembers it) */
	bool LogDue();

	const CtdWatchdogConfig &Config() const { return config; }

private:
	CtdWatchdogConfig config;
	std::deque<std::pair<double, uint64_t>> samples;
	std::optional<double> lastWarn;
	std::optional<double> lastLog;
};

/* "[ctd-watchdog] footprint 1834 MB (+12 MB/min over 3 min), NDI receive_send: 2 receivers, 1 send, 0 dropped" */
std::string CtdWatchdogLogLine(uint64_t bytes, double growthMbPerMin, int windowMinutes,
			       const CtdWatchdogNdiStatus &ndi);
