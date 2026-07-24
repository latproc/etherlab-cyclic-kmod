#!/bin/sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
module_path=${CW_EC_MODULE:-"$project_dir/kernel/cw_ethercat.ko"}
module_name=cw_ethercat

if [ "$(id -u)" -ne 0 ]; then
	echo "error: run as root; loading a kernel module requires privilege" >&2
	exit 1
fi
if [ ! -f "$module_path" ]; then
	echo "error: module not found: $module_path" >&2
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

ethercat slaves -v >"$tmp_dir/ethercat-before.txt"
"$project_dir/tools/cw_ec_capture_topology.sh" \
	>"$tmp_dir/topology-before.txt"
insmod "$module_path"
"$project_dir/tools/cw_ec_abi_test"
"$project_dir/tools/cw_ec_bus" >"$tmp_dir/cw-bus.txt"
rmmod "$module_name"
"$project_dir/tools/cw_ec_capture_topology.sh" \
	>"$tmp_dir/topology-after.txt"

cmp "$tmp_dir/topology-before.txt" "$tmp_dir/topology-after.txt"

awk '
	/^=== Master [0-9]+, Slave [0-9]+ ===$/ {
		position = $5
		gsub(/[^0-9]/, "", position)
	}
	/^  Vendor Id:/ { vendor = $3 }
	/^  Product code:/ { product = $3 }
	/^  Revision number:/ {
		revision = $3
		print position, vendor, product, revision
	}
' "$tmp_dir/ethercat-before.txt" >"$tmp_dir/ethercat-identities.txt"

awk '
	/^[0-9]+[[:space:]]/ {
		print $1, $3, $4, $5
	}
' "$tmp_dir/cw-bus.txt" >"$tmp_dir/cw-identities.txt"

cmp "$tmp_dir/ethercat-identities.txt" "$tmp_dir/cw-identities.txt"

slave_count=$(wc -l <"$tmp_dir/cw-identities.txt")
echo "PASS: $slave_count slave identities match; CLI topology unchanged"
