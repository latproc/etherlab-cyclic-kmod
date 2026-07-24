#!/bin/sh
#
# Deterministically fail every module-owned allocation reached by the
# non-activating SDO-staging and declarative-configuration preparation paths.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
module_path=${CW_EC_MODULE:-"$project_dir/kernel/cw_ethercat.ko"}
device=${CW_EC_DEVICE:-/dev/cw_ethercat0}
recipe=${CW_EC_RECIPE:-"$project_dir/tools/recipes/ed3l_velocity_pdo_pos29.txt"}
config=${CW_EC_CONFIG:-"$project_dir/tools/configs/ed3l_velocity_dc_pos29.conf"}
domain_config=${CW_EC_DOMAIN_CONFIG:-"$project_dir/tools/configs/el5152_pos3_with_absent_ed3l_pos29.conf"}
module_name=cw_ethercat

if [ "$(id -u)" -ne 0 ]; then
	echo "error: run as root" >&2
	exit 1
fi
if [ ! -f "$module_path" ] || [ ! -f "$recipe" ] ||
    [ ! -f "$config" ] || [ ! -f "$domain_config" ]; then
	echo "error: module, recipe, or configuration fixture is missing" >&2
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

run_failure_series()
{
	name=$1
	last_failure=$2
	shift 2
	fail=1
	while [ "$fail" -le "$last_failure" ]; do
		insmod "$module_path" test_fail_allocation="$fail"
		wait_for_device
		set +e
		"$@" >"$tmp_dir/$name-$fail.txt" 2>&1
		status=$?
		set -e
		if [ "$status" -eq 0 ]; then
			echo "error: $name allocation $fail unexpectedly succeeded" >&2
			exit 1
		fi
		verify_idle
		rmmod "$module_name"
		fail=$((fail + 1))
	done
}

"$project_dir/tools/cw_ec_capture_topology.sh" >"$tmp_dir/slaves-before.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-before.txt"

# One file-context allocation followed by one allocation per staged entry.
run_failure_series setup 22 \
	"$project_dir/tools/cw_ec_sdo" stage "$recipe" "$device"

insmod "$module_path" test_fail_allocation=23
wait_for_device
"$project_dir/tools/cw_ec_sdo" stage "$recipe" "$device"
verify_idle
rmmod "$module_name"

# Context, slave, two syncs, two PDOs, ten entries, DC configuration, and the
# implicit compatibility-domain node created during registration.
run_failure_series config 18 \
	"$project_dir/tools/cw_ec_config" prepare "$config" "$device"

insmod "$module_path" test_fail_allocation=19
wait_for_device
"$project_dir/tools/cw_ec_config" prepare "$config" "$device"
verify_idle
rmmod "$module_name"

# Context plus two domains, two assignments, two slaves, four syncs, eight
# PDOs, and 48 entries.
run_failure_series domains 67 \
	"$project_dir/tools/cw_ec_config" prepare "$domain_config" "$device"

insmod "$module_path" test_fail_allocation=68
wait_for_device
"$project_dir/tools/cw_ec_config" prepare "$domain_config" "$device"
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
	echo "error: fatal kernel diagnostic during allocation testing" >&2
	exit 1
fi

echo "New kernel warning/error lines:"
if [ -s "$tmp_dir/dmesg-new.txt" ]; then
	sed 's/^/  /' "$tmp_dir/dmesg-new.txt"
else
	echo "  none"
fi
echo "PASS: 107 injected allocation failures unwound; success boundaries passed; topology unchanged"
