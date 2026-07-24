#!/bin/sh
#
# Compare disarmed full-topology cyclic timing at idle, under same-CPU load,
# and under system CPU load. This is a bounded characterization harness, not
# a substitute for long-duration production timing acceptance.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
module_path=${CW_EC_MODULE:-"$project_dir/kernel/cw_ethercat.ko"}
module_name=cw_ethercat
config=${CW_EC_CONFIG:-"$project_dir/tools/configs/all34_captured_topology.conf"}
device=${CW_EC_DEVICE:-/dev/cw_ethercat0}
period=${CW_EC_TEST_PERIOD_NS:-1000000}
duration=${CW_EC_TEST_DURATION:-30}
repeat=${CW_EC_TEST_REPEAT:-3}
cycle_cpu=${CW_EC_TEST_CPU:-1}
fifo_priority=${CW_EC_TEST_FIFO_PRIORITY:-70}
maximum_lateness=${CW_EC_TEST_MAXIMUM_LATENESS_NS:-250000}

positive_integer()
{
	case "$2" in
		''|*[!0-9]*|0)
			echo "error: $1 must be a positive integer" >&2
			exit 2
			;;
	esac
}

if [ "$(id -u)" -ne 0 ]; then
	echo "error: run as root" >&2
	exit 1
fi
if [ "${CW_EC_MOTION_INHIBITED:-}" != YES ]; then
	echo "error: set CW_EC_MOTION_INHIBITED=YES only after motion is safely inhibited" >&2
	exit 2
fi
positive_integer CW_EC_TEST_PERIOD_NS "$period"
positive_integer CW_EC_TEST_DURATION "$duration"
positive_integer CW_EC_TEST_REPEAT "$repeat"
positive_integer CW_EC_TEST_MAXIMUM_LATENESS_NS "$maximum_lateness"
case "$cycle_cpu" in
	''|*[!0-9]*)
		echo "error: CW_EC_TEST_CPU must be a non-negative integer" >&2
		exit 2
		;;
esac
positive_integer CW_EC_TEST_FIFO_PRIORITY "$fifo_priority"
if [ ! -f "$module_path" ] || [ ! -f "$config" ]; then
	echo "error: module or configuration fixture is missing" >&2
	exit 1
fi
if ! command -v taskset >/dev/null || ! command -v sha256sum >/dev/null; then
	echo "error: taskset and sha256sum are required for controlled CPU load" >&2
	exit 1
fi
if grep -q "^$module_name " /proc/modules; then
	echo "error: $module_name is already loaded" >&2
	exit 1
fi

tmp_dir=$(mktemp -d)
load_pids=
controller_pid=
cleanup()
{
	if [ -n "$controller_pid" ]; then
		kill "$controller_pid" 2>/dev/null || true
		wait "$controller_pid" 2>/dev/null || true
	fi
	for pid in $load_pids; do
		kill "$pid" 2>/dev/null || true
	done
	for pid in $load_pids; do
		wait "$pid" 2>/dev/null || true
	done
	if grep -q "^$module_name " /proc/modules; then
		rmmod "$module_name"
	fi
	rm -rf -- "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

wait_for_device()
{
	i=0
	while [ "$i" -lt 100 ]; do
		[ -e "$device" ] && return 0
		sleep 0.05
		i=$((i + 1))
	done
	echo "error: device did not appear: $device" >&2
	return 1
}

stop_load()
{
	for pid in $load_pids; do
		kill "$pid" 2>/dev/null || true
	done
	for pid in $load_pids; do
		wait "$pid" 2>/dev/null || true
	done
	load_pids=
}

start_load()
{
	mode=$1
	load_pids=
	case "$mode" in
	baseline)
		return
		;;
	same-cpu)
		taskset -c "$cycle_cpu" sha256sum /dev/zero >/dev/null &
		load_pids=$!
		;;
	system)
		for cpu_dir in /sys/devices/system/cpu/cpu[0-9]*; do
			cpu=${cpu_dir##*cpu}
			[ -f "$cpu_dir/online" ] &&
				[ "$(cat "$cpu_dir/online")" -eq 0 ] && continue
			taskset -c "$cpu" sha256sum /dev/zero >/dev/null &
			load_pids="$load_pids $!"
		done
		;;
	*)
		echo "error: unknown load mode: $mode" >&2
		exit 1
		;;
	esac
}

field()
{
	printf '%s\n' "$1" | awk -v key="$2" '
		{
			for (i = 1; i <= NF; i++) {
				split($i, part, "=")
				if (part[1] == key) {
					print part[2]
					exit
				}
			}
	}'
}

print_failed_slaves()
{
	grep '^slave status:' "$1" |
		awk '
			{
				online = operational = valid = -1
				for (i = 1; i <= NF; i++) {
					split($i, part, "=")
					if (part[1] == "online")
						online = part[2]
					if (part[1] == "operational")
						operational = part[2]
					if (part[1] == "valid")
						valid = part[2]
				}
				if (online != 1 || operational != 1 || valid != 1)
					print
			}'
}

check_run()
{
	log=$1
	cycle_line=$(grep '^cycle status:' "$log")
	io_line=$(grep '^IO status:' "$log")
	errors=$(field "$cycle_line" errors)
	overruns=$(field "$cycle_line" overruns)
	lateness=$(field "$cycle_line" maximum_lateness)
	wc_state=$(field "$cycle_line" wc_state)
	healthy=$(field "$io_line" healthy)
	armed=$(field "$io_line" armed)
	operational=$(field "$io_line" operational)
	configured=$(field "$io_line" configured)

	if [ "$errors" -ne 0 ] || [ "$overruns" -ne 0 ] ||
	    [ "$lateness" -gt "$maximum_lateness" ] ||
	    [ "$wc_state" -ne 2 ] || [ "$healthy" -ne 1 ] ||
	    [ "$armed" -ne 0 ] || [ "$operational" -ne "$configured" ]; then
		echo "error: timing acceptance condition failed in $log" >&2
		grep -E '^(cycle status:|IO status:|domain status:)' "$log" >&2
		print_failed_slaves "$log" >&2
		return 1
	fi
	if grep '^domain status:' "$log" |
		awk '
			{
				wc = valid = -1
				for (i = 1; i <= NF; i++) {
					split($i, part, "=")
					if (part[1] == "wc_state")
						wc = part[2]
					if (part[1] == "valid")
						valid = part[2]
				}
				if (wc != 2 || valid != 1)
					exit 1
				count++
			}
			END { if (!count) exit 1 }'; then
		:
	else
		echo "error: incomplete or invalid domain in $log" >&2
		return 1
	fi
	printf '  %s\n' "$cycle_line"
	printf '  %s\n' "$io_line"
	grep '^domain status:' "$log" | sed 's/^/  /'
}

"$project_dir/tools/cw_ec_config" check "$config"
"$project_dir/tools/cw_ec_capture_topology.sh" >"$tmp_dir/slaves-before.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-before.txt"
insmod "$module_path" cycle_cpu="$cycle_cpu" \
	cycle_fifo_priority="$fifo_priority"
wait_for_device

echo "Timing criteria: errors=0 overruns=0 maximum_lateness<=${maximum_lateness}ns"
echo "  aggregate/domain WC complete, every domain valid, all configured slaves operational"
echo "  period=${period}ns duration=${duration}s repeat=$repeat CPU=$cycle_cpu FIFO=$fifo_priority"

for mode in baseline same-cpu system; do
	i=1
	while [ "$i" -le "$repeat" ]; do
		log="$tmp_dir/$mode-$i.log"
		echo "$mode trial $i/$repeat"
		"$project_dir/tools/cw_ec_config" cycle-strict \
			"$config" "$period" "$duration" "$device" >"$log" &
		controller_pid=$!
		ready_attempts=0
		while ! grep -q '^READY: strict-health' "$log"; do
			if ! kill -0 "$controller_pid" 2>/dev/null; then
				wait "$controller_pid" || true
				controller_pid=
				echo "error: strict timing startup failed in $log" >&2
				print_failed_slaves "$log" >&2
				exit 1
			fi
			if [ "$ready_attempts" -ge 120 ]; then
				echo "error: strict timing readiness timed out in $log" >&2
				exit 1
			fi
			sleep 0.05
			ready_attempts=$((ready_attempts + 1))
		done
		start_load "$mode"
		if ! wait "$controller_pid"; then
			controller_pid=
			stop_load
			echo "error: strict timing cycle failed in $log" >&2
			grep -E '^(cycle status:|IO status:|domain status:)' \
				"$log" >&2 || true
			print_failed_slaves "$log" >&2
			exit 1
		fi
		controller_pid=
		stop_load
		check_run "$log"
		i=$((i + 1))
	done
done

rmmod "$module_name"
"$project_dir/tools/cw_ec_capture_topology.sh" >"$tmp_dir/slaves-after.txt"
cmp "$tmp_dir/slaves-before.txt" "$tmp_dir/slaves-after.txt"
ethercat master >"$tmp_dir/master.txt"
grep -q 'Phase: Idle' "$tmp_dir/master.txt"
grep -q 'Active: no' "$tmp_dir/master.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-after.txt"
before_lines=$(wc -l <"$tmp_dir/dmesg-before.txt")
sed -n "$((before_lines + 1)),\$p" "$tmp_dir/dmesg-after.txt" \
	>"$tmp_dir/dmesg-new.txt"
if grep -E 'BUG:|Oops:|general protection fault|use-after-free|KASAN:|kernel NULL pointer|hung task' \
	"$tmp_dir/dmesg-new.txt"; then
	echo "error: fatal kernel diagnostic during timing test" >&2
	exit 1
fi
echo "New kernel warning/error lines:"
if [ -s "$tmp_dir/dmesg-new.txt" ]; then
	sed 's/^/  /' "$tmp_dir/dmesg-new.txt"
else
	echo "  none"
fi

trap - EXIT HUP INT TERM
rm -rf -- "$tmp_dir"
echo "PASS: all disarmed timing trials met the declared criteria; topology unchanged"
