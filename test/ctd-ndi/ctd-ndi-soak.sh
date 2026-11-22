#!/bin/bash
# CTD OBS: NDI soak test for a real Mac (docs/ctd/ndi.md, "Soak test").
#
# CTD OBS must already be running with Receive + Send, 2 NDI® Sources showing video in the program scene and the
# program send on (2 receivers, 1 send). This script then watches it for 60 minutes:
#
#   every 60 s   the footprint CTD OBS's memory watchdog logs (phys_footprint, from inside the process: includes
#                IOSurfaces and graphics memory), ps RSS as a cross-check, and the NDI counts, into a CSV
#   PASS         highest footprint - first footprint < 300 MB, and 2 receivers + 1 send in every minute
#   FAIL         anything else, including CTD OBS quitting or the watchdog line going stale
#
# Usage: test/ctd-ndi/ctd-ndi-soak.sh [--minutes 60] [--limit-mb 300] [--receivers 2] [--sends 1]
#                                     [--pid PID] [--log FILE] [--csv FILE]
# Exit status: 0 PASS, 1 FAIL, 2 could not start.
#
# Runs with macOS's own /bin/bash (3.2). Needs no sudo: the numbers come from CTD OBS's own log.

set -u

minutes=60
limit_mb=300
want_receivers=2
want_sends=1
pid=''
log=''
csv=''
interval="${CTD_SOAK_INTERVAL:-60}"   # seconds between samples; only shortened for testing the script
wait_ready="${CTD_SOAK_WAIT_READY:-300}"

while [ $# -gt 0 ]; do
	case "$1" in
	--minutes) minutes="$2"; shift 2 ;;
	--limit-mb) limit_mb="$2"; shift 2 ;;
	--receivers) want_receivers="$2"; shift 2 ;;
	--sends) want_sends="$2"; shift 2 ;;
	--pid) pid="$2"; shift 2 ;;
	--log) log="$2"; shift 2 ;;
	--csv) csv="$2"; shift 2 ;;
	-h|--help) sed -n '2,18p' "$0"; exit 0 ;;
	*) echo "Unknown option: $1" >&2; exit 2 ;;
	esac
done

say() { printf '%s  %s\n' "$(date '+%H:%M:%S')" "$*"; }
die() { say "CANNOT START: $*"; exit 2; }

# ------------------------------------------------------------------------------------------------ preflight

if [ "$(uname -s)" != Darwin ] && [ "${CTD_SOAK_ALLOW_NON_MAC:-0}" != 1 ]; then
	die "this is the Mac soak test (uname: $(uname -s))"
fi

# The 2026-11-22 incident: NDI Tools' system pieces. The soak test must run without them.
found_pieces=''
for piece in \
	"/Library/CoreMediaIO/Plug-Ins/DAL/NDIVideoOut.plugin" \
	"/Library/Audio/Plug-Ins/HAL/NDIAudio.driver" \
	"/Library/Audio/Plug-Ins/HAL/NDIAudioOut.driver" \
	"/Library/LaunchDaemons/com.newtek.cmio.DPA.NDI.plist" \
	"/Library/Application Support/NewTek/NDI/HX_Driver"; do
	if [ -e "$piece" ]; then
		found_pieces="${found_pieces}    ${piece}
"
	fi
done
if [ -n "$found_pieces" ]; then
	say "NDI Tools pieces are installed system-wide:"
	printf '%s' "$found_pieces"
	die "remove NDI Tools first (keep only the NDI 6 Runtime): the soak test must run without them"
fi

if [ -z "$pid" ]; then
	pids="$(pgrep -f 'CTD OBS.app/Contents/MacOS/' 2>/dev/null | tr '\n' ' ')"
	set -- $pids
	if [ $# -eq 0 ]; then
		die "CTD OBS isn't running"
	elif [ $# -gt 1 ]; then
		die "more than one CTD OBS is running ($pids); pass --pid"
	fi
	pid="$1"
fi
kill -0 "$pid" 2>/dev/null || die "process $pid isn't running"

if [ -z "$log" ]; then
	logdir="$HOME/Library/Application Support/ctd-obs/logs"
	log="$(ls -t "$logdir"/*.txt 2>/dev/null | head -n 1)"
	[ -n "$log" ] || die "no CTD OBS log in $logdir"
fi
[ -r "$log" ] || die "can't read $log"

if [ -z "$csv" ]; then
	if [ -d "$HOME/Desktop" ]; then
		csv="$HOME/Desktop/ctd-ndi-soak-$(date '+%Y%m%d-%H%M').csv"
	else
		csv="./ctd-ndi-soak-$(date '+%Y%m%d-%H%M').csv"
	fi
fi

say "CTD OBS pid $pid"
say "log $log"
say "CSV $csv"

# ------------------------------------------------------------------------------------------------ helpers

# The newest watchdog line and how many there are
watchdog_lines() { grep -c '\[ctd-watchdog\] footprint ' "$log" 2>/dev/null || true; }
last_watchdog() { grep '\[ctd-watchdog\] footprint ' "$log" 2>/dev/null | tail -n 1; }

# field <line> <regex with one group>
field() { printf '%s\n' "$1" | sed -nE "s/.*$2.*/\\1/p" | head -n 1; }

parse() {
	line="$1"
	fp_mb="$(field "$line" 'footprint ([0-9]+) MB')"
	if printf '%s' "$line" | grep -q 'NDI off'; then
		receivers=0; sends=0; dropped=0
	else
		receivers="$(field "$line" ' ([0-9]+) receivers?,')"
		sends="$(field "$line" ' ([0-9]+) sends?,')"
		dropped="$(field "$line" ' ([0-9]+) dropped')"
	fi
	paused=0
	printf '%s' "$line" | grep -q '(paused)' && paused=1
	: "${fp_mb:=}" "${receivers:=}" "${sends:=}" "${dropped:=0}"
}

rss_mb() {
	kb="$(ps -o rss= -p "$pid" 2>/dev/null | tr -d ' ')"
	[ -n "$kb" ] && echo $((kb / 1024)) || echo ''
}

# ------------------------------------------------------------------------------------------------ wait until ready

if command -v caffeinate > /dev/null 2>&1; then
	caffeinate -dimsu -w $$ &
	say "caffeinate: the Mac won't sleep while this runs"
fi

say "Waiting for the watchdog to report ${want_receivers} receivers and ${want_sends} send (up to ${wait_ready} s)…"
waited=0
while :; do
	kill -0 "$pid" 2>/dev/null || die "CTD OBS quit while waiting"
	parse "$(last_watchdog)"
	if [ "$receivers" = "$want_receivers" ] && [ "$sends" = "$want_sends" ] && [ "$paused" = 0 ]; then
		break
	fi
	if [ "$waited" -ge "$wait_ready" ]; then
		die "the watchdog shows receivers=${receivers:-?} sends=${sends:-?} paused=${paused}; set up 2 NDI Sources (showing video) and the program send, see docs/ctd/ndi.md"
	fi
	sleep 5
	waited=$((waited + 5))
done
say "Ready. Soak test: ${minutes} min, pass if growth < ${limit_mb} MB."

# ------------------------------------------------------------------------------------------------ the soak

echo 'minute,time,footprint_mb,rss_mb,receivers,sends,dropped,growth_mb' > "$csv"

base=''
max=0
last=0
bad_counts=0
stale=0
stale_max=0
seen_lines="$(watchdog_lines)"
result=PASS
reasons=''

minute=0
while [ "$minute" -le "$minutes" ]; do
	if ! kill -0 "$pid" 2>/dev/null; then
		result=FAIL
		reasons="${reasons}CTD OBS quit at minute ${minute}. "
		say "CTD OBS is gone (minute ${minute})"
		break
	fi

	count="$(watchdog_lines)"
	if [ "$minute" -gt 0 ] && [ "$count" -le "$seen_lines" ]; then
		stale=$((stale + 1))
	else
		stale=0
	fi
	[ "$stale" -gt "$stale_max" ] && stale_max=$stale
	seen_lines=$count

	parse "$(last_watchdog)"
	rss="$(rss_mb)"
	if [ -z "$fp_mb" ]; then
		result=FAIL
		reasons="${reasons}No watchdog line at minute ${minute}. "
		break
	fi
	[ -z "$base" ] && base=$fp_mb
	[ "$fp_mb" -gt "$max" ] && max=$fp_mb
	last=$fp_mb
	growth=$((fp_mb - base))

	if [ "$receivers" != "$want_receivers" ] || [ "$sends" != "$want_sends" ] || [ "$paused" != 0 ]; then
		bad_counts=$((bad_counts + 1))
		say "minute ${minute}: receivers=${receivers} sends=${sends} paused=${paused} (want ${want_receivers}/${want_sends})"
	fi

	echo "${minute},$(date '+%Y-%m-%dT%H:%M:%S'),${fp_mb},${rss},${receivers},${sends},${dropped},${growth}" >> "$csv"
	say "minute ${minute}/${minutes}: footprint ${fp_mb} MB (${growth} MB since start, peak +$((max - base))), RSS ${rss:-?} MB, NDI ${receivers} receivers ${sends} send, ${dropped} dropped"

	if [ $((max - base)) -ge "$limit_mb" ]; then
		result=FAIL
		reasons="${reasons}Grew $((max - base)) MB (limit ${limit_mb}) by minute ${minute}. "
		break
	fi

	[ "$minute" -ge "$minutes" ] && break
	minute=$((minute + 1))
	sleep "$interval"
done

if [ "$bad_counts" -gt 0 ]; then
	result=FAIL
	reasons="${reasons}NDI counts were wrong in ${bad_counts} minute(s). "
fi
if [ "$stale_max" -ge 3 ]; then
	result=FAIL
	reasons="${reasons}The watchdog line didn't update for ${stale_max} minutes in a row. "
fi
if [ "$result" = PASS ] && [ "$minute" -lt "$minutes" ]; then
	result=FAIL
	reasons="${reasons}Stopped at minute ${minute} of ${minutes}. "
fi

echo
say "==== CTD OBS NDI soak test: ${result} ===="
say "start ${base:-?} MB, peak ${max} MB (+$(( ${max:-0} - ${base:-0} )) MB), end ${last} MB, limit +${limit_mb} MB"
say "minutes ${minute}/${minutes}, receivers ${want_receivers}, sends ${want_sends}"
[ -n "$reasons" ] && say "why: ${reasons}"
say "CSV: ${csv}"
say "log: ${log}"
[ "$result" = PASS ] && exit 0 || exit 1
