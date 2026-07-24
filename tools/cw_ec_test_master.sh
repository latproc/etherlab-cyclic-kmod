#!/bin/sh
#
# Exercise the minimal master acquisition/release probe and verify that the
# EtherLab CLI has the same usability after the probe is unloaded.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
module_path=${CW_EC_PROBE_MODULE:-"$project_dir/kernel/cw_ethercat_probe.ko"}
module_name=cw_ethercat_probe
repeat=${CW_EC_TEST_REPEAT:-1}

case "$repeat" in
	''|*[!0-9]*|0)
		echo "error: CW_EC_TEST_REPEAT must be a positive integer" >&2
		exit 2
		;;
esac

if [ "$(id -u)" -ne 0 ]; then
	echo "error: run as root; loading a kernel module requires privilege" >&2
	exit 1
fi

if [ ! -f "$module_path" ]; then
	echo "error: module not found: $module_path" >&2
	echo "build it first with: make" >&2
	exit 1
fi

if grep -q "^$module_name " /proc/modules; then
	echo "error: $module_name is already loaded" >&2
	exit 1
fi

cleanup()
{
	if grep -q "^$module_name " /proc/modules; then
		rmmod "$module_name"
	fi
}
trap cleanup EXIT HUP INT TERM

run_master_status()
{
	output_file=$1
	if command -v ethercat >/dev/null 2>&1; then
		set +e
		ethercat master >"$output_file" 2>&1
		status=$?
		set -e
	else
		printf '%s\n' "ethercat command not installed" >"$output_file"
		status=127
	fi
	return "$status"
}

tmp_dir=$(mktemp -d)
trap 'cleanup; rm -rf -- "$tmp_dir"' EXIT HUP INT TERM

set +e
run_master_status "$tmp_dir/before.txt"
before_status=$?
set -e

echo "EtherLab master status before probe (exit $before_status):"
sed 's/^/  /' "$tmp_dir/before.txt"

i=1
while [ "$i" -le "$repeat" ]; do
	echo "Probe iteration $i/$repeat"
	insmod "$module_path"
	rmmod "$module_name"
	i=$((i + 1))
done

set +e
run_master_status "$tmp_dir/after.txt"
after_status=$?
set -e

echo "EtherLab master status after probe (exit $after_status):"
sed 's/^/  /' "$tmp_dir/after.txt"

if [ "$before_status" -ne "$after_status" ]; then
	echo "error: ethercat master exit status changed after the probe" >&2
	exit 1
fi

echo "Recent probe log lines:"
dmesg | grep "$module_name" | tail -n 20 || true

echo "PASS: $repeat acquire/release iteration(s); CLI usability unchanged"
