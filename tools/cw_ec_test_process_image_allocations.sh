#!/bin/sh
#
# Fail each of the six copied process-image allocations before master
# activation, then verify the first non-failing boundary with a zero-output
# cycle.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
module_path=${CW_EC_MODULE:-"$project_dir/kernel/cw_ethercat.ko"}
module_name=cw_ethercat
device=${CW_EC_DEVICE:-/dev/cw_ethercat0}
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
cleanup()
{
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

verify_idle()
{
	ethercat master >"$tmp_dir/master.txt"
	grep -q 'Phase: Idle' "$tmp_dir/master.txt"
	grep -q 'Active: no' "$tmp_dir/master.txt"
}

"$project_dir/tools/cw_ec_capture_topology.sh" >"$tmp_dir/slaves-before.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-before.txt"

# Allocations 1-18 are the file context, pending records, and API 0.12 implicit
# compatibility-domain node. Allocations 19-24 are two input images, two output
# images, the output mask, and the output update mask.
fail=19
while [ "$fail" -le 24 ]; do
	insmod "$module_path" test_fail_allocation="$fail"
	wait_for_device
	set +e
	"$project_dir/tools/cw_ec_config" cycle "$config" "$period" 1 "$device" \
		>"$tmp_dir/failure-$fail.txt" 2>&1
	status=$?
	set -e
	if [ "$status" -eq 0 ] ||
	    ! grep -q 'activation failed: Cannot allocate memory' \
		    "$tmp_dir/failure-$fail.txt"; then
		echo "error: process-image allocation $fail did not fail as expected" >&2
		sed 's/^/  /' "$tmp_dir/failure-$fail.txt" >&2
		exit 1
	fi
	verify_idle
	rmmod "$module_name"
	fail=$((fail + 1))
done

# Task construction happens after EtherLab activation. Its failure must
# deactivate the master, free all six process-image buffers, and invalidate
# every EtherLab-owned configuration/domain pointer before close.
insmod "$module_path" test_fail_cycle_thread=1
wait_for_device
set +e
"$project_dir/tools/cw_ec_config" cycle "$config" "$period" 1 "$device" \
	>"$tmp_dir/thread-failure.txt" 2>&1
status=$?
set -e
if [ "$status" -eq 0 ] ||
    ! grep -q 'activation failed: Cannot allocate memory' \
	    "$tmp_dir/thread-failure.txt"; then
	echo "error: cyclic task construction did not fail as expected" >&2
	sed 's/^/  /' "$tmp_dir/thread-failure.txt" >&2
	exit 1
fi
verify_idle
rmmod "$module_name"

# Allocation 25 is beyond every module-owned allocation reached by this
# fixture. It must complete a normal zero-output cycle, reach a healthy bus,
# and teardown.
insmod "$module_path" test_fail_allocation=25
wait_for_device
"$project_dir/tools/cw_ec_config" cycle "$config" "$period" 8 "$device" \
	>"$tmp_dir/success-boundary.txt"
cat "$tmp_dir/success-boundary.txt"
grep -q 'IO status: .* healthy=1 ' "$tmp_dir/success-boundary.txt"
verify_idle
rmmod "$module_name"

"$project_dir/tools/cw_ec_capture_topology.sh" >"$tmp_dir/slaves-after.txt"
cmp "$tmp_dir/slaves-before.txt" "$tmp_dir/slaves-after.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-after.txt"
before_lines=$(wc -l <"$tmp_dir/dmesg-before.txt")
sed -n "$((before_lines + 1)),\$p" "$tmp_dir/dmesg-after.txt" \
	>"$tmp_dir/dmesg-new.txt"

if grep -E 'BUG:|Oops:|general protection fault|use-after-free|KASAN:|kernel NULL pointer|hung task' \
	"$tmp_dir/dmesg-new.txt"; then
	echo "error: fatal kernel diagnostic during process-image allocation testing" >&2
	exit 1
fi

echo "New kernel warning/error lines:"
if [ -s "$tmp_dir/dmesg-new.txt" ]; then
	sed 's/^/  /' "$tmp_dir/dmesg-new.txt"
else
	echo "  none"
fi
echo "PASS: all six process-image and cyclic-task construction failures unwound; success boundary passed; topology unchanged"
