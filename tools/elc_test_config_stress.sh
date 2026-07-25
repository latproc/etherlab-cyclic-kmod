#!/bin/sh
#
# Exercise every pending-configuration hard limit and synchronous reset without
# applying configuration or activating the EtherLab master.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
module_path=${ELC_MODULE:-"$project_dir/kernel/elc_ethercat.ko"}
module_name=elc_ethercat
device=${ELC_DEVICE:-/dev/elc_ethercat0}
iterations=${ELC_STRESS_ITERATIONS:-10}

case "$iterations" in
	''|*[!0-9]*|0)
		echo "error: ELC_STRESS_ITERATIONS must be a positive integer" >&2
		exit 2
		;;
esac
if [ "$iterations" -gt 100 ]; then
	echo "error: ELC_STRESS_ITERATIONS must not exceed 100" >&2
	exit 2
fi
if [ "$(id -u)" -ne 0 ]; then
	echo "error: run as root" >&2
	exit 1
fi
if [ ! -f "$module_path" ]; then
	echo "error: module is missing: $module_path" >&2
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

"$project_dir/tools/elc_capture_topology.sh" >"$tmp_dir/slaves-before.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-before.txt"
insmod "$module_path"

i=0
while [ "$i" -lt 100 ]; do
	[ -e "$device" ] && break
	sleep 0.05
	i=$((i + 1))
done
if [ ! -e "$device" ]; then
	echo "error: device did not appear: $device" >&2
	exit 1
fi

"$project_dir/tools/elc_config_stress" "$iterations" "$device"

ethercat master >"$tmp_dir/master-after.txt"
grep -q 'Phase: Idle' "$tmp_dir/master-after.txt"
grep -q 'Active: no' "$tmp_dir/master-after.txt"
rmmod "$module_name"

"$project_dir/tools/elc_capture_topology.sh" >"$tmp_dir/slaves-after.txt"
cmp "$tmp_dir/slaves-before.txt" "$tmp_dir/slaves-after.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-after.txt"
before_lines=$(wc -l <"$tmp_dir/dmesg-before.txt")
sed -n "$((before_lines + 1)),\$p" "$tmp_dir/dmesg-after.txt" \
	>"$tmp_dir/dmesg-new.txt"

if grep -E 'BUG:|Oops:|general protection fault|use-after-free|KASAN:|kernel NULL pointer|hung task' \
	"$tmp_dir/dmesg-new.txt"; then
	echo "error: fatal kernel diagnostic during configuration stress" >&2
	exit 1
fi

echo "New kernel warning/error lines:"
if [ -s "$tmp_dir/dmesg-new.txt" ]; then
	sed 's/^/  /' "$tmp_dir/dmesg-new.txt"
else
	echo "  none"
fi
echo "PASS: $iterations maximum pending configuration iteration(s); master idle; topology unchanged"
