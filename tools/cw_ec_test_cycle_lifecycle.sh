#!/bin/sh
#
# Repeat the current copied-image/zero-arm lifecycle. This test never requests
# a nonzero output, but it changes EtherCAT PDO configuration and therefore
# requires the site commissioning state and explicit motion-inhibit acknowledgement.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
module_path=${CW_EC_MODULE:-"$project_dir/kernel/cw_ethercat.ko"}
module_name=cw_ethercat
config=${CW_EC_CONFIG:-"$project_dir/tools/configs/ed3l_velocity_dc_pos29.conf"}
repeat=${CW_EC_TEST_REPEAT:-5}
duration=${CW_EC_TEST_DURATION:-2}
period=${CW_EC_TEST_PERIOD_NS:-1000000}

case "$repeat" in
	''|*[!0-9]*|0)
		echo "error: CW_EC_TEST_REPEAT must be a positive integer" >&2
		exit 2
		;;
esac
case "$duration" in
	''|*[!0-9]*|0)
		echo "error: CW_EC_TEST_DURATION must be a positive integer" >&2
		exit 2
		;;
esac
case "$period" in
	''|*[!0-9]*|0)
		echo "error: CW_EC_TEST_PERIOD_NS must be a positive integer" >&2
		exit 2
		;;
esac
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

cycle_tasks()
{
	ps -e -o comm= | awk '$1 == "cw_ec_cycle" { count++ } END { print count + 0 }'
}

before_tasks=$(cycle_tasks)
"$project_dir/tools/cw_ec_capture_topology.sh" >"$tmp_dir/slaves-before.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-before.txt"
insmod "$module_path"

i=1
while [ "$i" -le "$repeat" ]; do
	echo "API lifecycle iteration $i/$repeat"
	"$project_dir/tools/cw_ec_config" cycle-zero-arm \
		"$config" "$period" "$duration" \
		>"$tmp_dir/cycle-$i.txt"
	if [ "$(cycle_tasks)" -ne "$before_tasks" ]; then
		echo "error: cyclic task count changed after iteration $i" >&2
		exit 1
	fi
	ethercat master >"$tmp_dir/master-$i.txt"
	if ! grep -q 'Phase: Idle' "$tmp_dir/master-$i.txt" ||
	    ! grep -q 'Active: no' "$tmp_dir/master-$i.txt"; then
		echo "error: EtherLab master was not idle after iteration $i" >&2
		exit 1
	fi
	i=$((i + 1))
done

rmmod "$module_name"
"$project_dir/tools/cw_ec_capture_topology.sh" >"$tmp_dir/slaves-after.txt"
cmp "$tmp_dir/slaves-before.txt" "$tmp_dir/slaves-after.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-after.txt"

before_lines=$(wc -l <"$tmp_dir/dmesg-before.txt")
sed -n "$((before_lines + 1)),\$p" "$tmp_dir/dmesg-after.txt" \
	>"$tmp_dir/dmesg-new.txt"

echo "New kernel warning/error lines:"
if [ -s "$tmp_dir/dmesg-new.txt" ]; then
	sed 's/^/  /' "$tmp_dir/dmesg-new.txt"
else
	echo "  none"
fi
echo "PASS: $repeat API lifecycle iteration(s); no cyclic task leak; topology unchanged"
