#!/bin/sh
#
# Kill a controller while it holds an explicitly armed all-zero output shadow.
# File release must zero-gate, stop the cyclic thread, and release master 0.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
module_path=${CW_EC_MODULE:-"$project_dir/kernel/cw_ethercat.ko"}
module_name=cw_ethercat
config=${CW_EC_CONFIG:-"$project_dir/tools/configs/ed3l_velocity_dc_pos29.conf"}
period=${CW_EC_TEST_PERIOD_NS:-1000000}

if [ "${CW_EC_MOTION_INHIBITED:-}" != YES ]; then
	echo "error: set CW_EC_MOTION_INHIBITED=YES only after motion is safely inhibited" >&2
	exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "error: run as root" >&2
	exit 1
fi
if [ ! -f "$module_path" ] || [ ! -f "$config" ]; then
	echo "error: module or configuration fixture is missing" >&2
	exit 1
fi
if grep -q "^$module_name " /proc/modules; then
	echo "error: $module_name is already loaded" >&2
	exit 1
fi

tmp_dir=$(mktemp -d)
controller_pid=
cleanup()
{
	if [ -n "$controller_pid" ] && kill -0 "$controller_pid" 2>/dev/null; then
		kill -KILL "$controller_pid"
		wait "$controller_pid" 2>/dev/null || true
	fi
	if grep -q "^$module_name " /proc/modules; then
		rmmod "$module_name"
	fi
	rm -rf -- "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

cycle_tasks()
{
	ps -e -o comm= | awk '$1 == "cw_ec_cycle" { count++ } END { print count + 0 }'
}

before_tasks=$(cycle_tasks)
"$project_dir/tools/cw_ec_capture_topology.sh" >"$tmp_dir/slaves-before.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-before.txt"
insmod "$module_path"

stdbuf -oL -eL "$project_dir/tools/cw_ec_config" cycle-zero-hold \
	"$config" "$period" 60 >"$tmp_dir/controller.txt" 2>&1 &
controller_pid=$!

ready=0
i=0
while [ "$i" -lt 200 ]; do
	if grep -q '^READY: zero-output shadow armed' "$tmp_dir/controller.txt"; then
		ready=1
		break
	fi
	if ! kill -0 "$controller_pid" 2>/dev/null; then
		break
	fi
	sleep 0.05
	i=$((i + 1))
done
if [ "$ready" -ne 1 ]; then
	echo "error: controller did not reach zero-armed hold state" >&2
	sed 's/^/  /' "$tmp_dir/controller.txt" >&2
	exit 1
fi

echo "Killing zero-armed controller process $controller_pid"
kill -KILL "$controller_pid"
set +e
wait "$controller_pid"
wait_status=$?
set -e
controller_pid=
if [ "$wait_status" -ne 137 ]; then
	echo "error: expected killed controller status 137, got $wait_status" >&2
	exit 1
fi

if [ "$(cycle_tasks)" -ne "$before_tasks" ]; then
	echo "error: cyclic task count changed after controller death" >&2
	exit 1
fi
ethercat master >"$tmp_dir/master-after.txt"
if ! grep -q 'Phase: Idle' "$tmp_dir/master-after.txt" ||
    ! grep -q 'Active: no' "$tmp_dir/master-after.txt"; then
	echo "error: EtherLab master was not idle after controller death" >&2
	exit 1
fi

rmmod "$module_name"
"$project_dir/tools/cw_ec_capture_topology.sh" >"$tmp_dir/slaves-after.txt"
cmp "$tmp_dir/slaves-before.txt" "$tmp_dir/slaves-after.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-after.txt"
before_lines=$(wc -l <"$tmp_dir/dmesg-before.txt")
sed -n "$((before_lines + 1)),\$p" "$tmp_dir/dmesg-after.txt" \
	>"$tmp_dir/dmesg-new.txt"

if grep -E 'BUG:|Oops:|general protection fault|use-after-free|KASAN:|kernel NULL pointer|hung task' \
	"$tmp_dir/dmesg-new.txt"; then
	echo "error: fatal kernel diagnostic followed controller death" >&2
	exit 1
fi

echo "New kernel warning/error lines:"
if [ -s "$tmp_dir/dmesg-new.txt" ]; then
	sed 's/^/  /' "$tmp_dir/dmesg-new.txt"
else
	echo "  none"
fi
echo "PASS: killed zero-armed controller; no cyclic task leak; master released; topology unchanged"
