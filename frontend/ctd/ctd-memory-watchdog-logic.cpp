/******************************************************************************
    CTD OBS: the memory watchdog's maths (ctd-memory-watchdog-logic.hpp).

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.
******************************************************************************/

#include "ctd-memory-watchdog-logic.hpp"

#include <cmath>
#include <cstdio>

void CtdWatchdogLogic::AddSample(double tSec, uint64_t bytes)
{
	if (!samples.empty() && tSec <= samples.back().first) {
		return;
	}
	samples.emplace_back(tSec, bytes);

	/* keep the sustain window plus a minute: enough for every question asked */
	const double keep = (config.sustainMinutes + 1) * 60.0 + 30.0;
	while (samples.size() > 2 && samples.front().first < tSec - keep && samples[1].first <= tSec - keep) {
		samples.pop_front();
	}
}

std::optional<uint64_t> CtdWatchdogLogic::BytesAt(double tSec) const
{
	std::optional<uint64_t> found;
	for (const auto &sample : samples) {
		if (sample.first > tSec + 1e-9) {
			break;
		}
		found = sample.second;
	}
	return found;
}

std::optional<uint64_t> CtdWatchdogLogic::LatestBytes() const
{
	if (samples.empty()) {
		return std::nullopt;
	}
	return samples.back().second;
}

double CtdWatchdogLogic::LatestTime() const
{
	return samples.empty() ? 0.0 : samples.back().first;
}

double CtdWatchdogLogic::GrowthMbPerMin(double windowSec) const
{
	if (samples.size() < 2) {
		return 0.0;
	}
	const double now = samples.back().first;
	double from = now - windowSec;
	if (from < samples.front().first) {
		from = samples.front().first;
	}
	const auto start = BytesAt(from);
	if (!start) {
		return 0.0;
	}
	/* the time of the sample actually used */
	double startTime = samples.front().first;
	for (const auto &sample : samples) {
		if (sample.first > from + 1e-9) {
			break;
		}
		startTime = sample.first;
	}
	const double span = now - startTime;
	if (span < 60.0) {
		return 0.0;
	}
	const double deltaMb = (static_cast<double>(samples.back().second) - static_cast<double>(*start)) /
			       static_cast<double>(kCtdMiB);
	return deltaMb / (span / 60.0);
}

bool CtdWatchdogLogic::SustainedGrowth() const
{
	if (samples.empty() || config.sustainMinutes <= 0) {
		return false;
	}
	const double now = samples.back().first;
	const double needMb = config.rateMbPerMin;
	for (int k = 0; k < config.sustainMinutes; k++) {
		const auto end = BytesAt(now - 60.0 * k);
		const auto begin = BytesAt(now - 60.0 * (k + 1));
		if (!end || !begin) {
			return false; /* not enough history yet */
		}
		const double grewMb =
			(static_cast<double>(*end) - static_cast<double>(*begin)) / static_cast<double>(kCtdMiB);
		if (grewMb < needMb) {
			return false;
		}
	}
	return true;
}

bool CtdWatchdogLogic::OverLimit() const
{
	const auto bytes = LatestBytes();
	return bytes && config.limitMb > 0 && *bytes > config.limitMb * kCtdMiB;
}

CtdWatchdogWarning CtdWatchdogLogic::Evaluate()
{
	if (samples.empty()) {
		return CtdWatchdogWarning::None;
	}
	CtdWatchdogWarning warning = CtdWatchdogWarning::None;
	if (SustainedGrowth()) {
		warning = CtdWatchdogWarning::Growth;
	} else if (OverLimit()) {
		warning = CtdWatchdogWarning::Limit;
	}
	if (warning == CtdWatchdogWarning::None) {
		return warning;
	}
	const double now = samples.back().first;
	if (lastWarn && now - *lastWarn < config.warnIntervalSec) {
		return CtdWatchdogWarning::None;
	}
	lastWarn = now;
	return warning;
}

bool CtdWatchdogLogic::LogDue()
{
	if (samples.empty()) {
		return false;
	}
	const double now = samples.back().first;
	if (lastLog && now - *lastLog < config.logIntervalSec - 1e-9) {
		return false;
	}
	lastLog = now;
	return true;
}

std::string CtdWatchdogLogLine(uint64_t bytes, double growthMbPerMin, int windowMinutes,
			       const CtdWatchdogNdiStatus &ndi)
{
	char rate[64];
	const long long rounded = std::llround(growthMbPerMin);
	snprintf(rate, sizeof(rate), "%s%lld MB/min over %d min", rounded >= 0 ? "+" : "", rounded, windowMinutes);

	char ndiText[160];
	if (ndi.mode == "off") {
		snprintf(ndiText, sizeof(ndiText), "NDI off");
	} else {
		snprintf(ndiText, sizeof(ndiText), "NDI %s%s: %d sender%s, %llu dropped", ndi.mode.c_str(),
			 ndi.paused ? " (paused)" : "", ndi.senders, ndi.senders == 1 ? "" : "s",
			 static_cast<unsigned long long>(ndi.droppedFrames));
	}

	char line[320];
	snprintf(line, sizeof(line), "[ctd-watchdog] footprint %llu MB (%s), %s",
		 static_cast<unsigned long long>((bytes + kCtdMiB / 2) / kCtdMiB), rate, ndiText);
	return line;
}
