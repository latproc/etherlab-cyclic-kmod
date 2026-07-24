#!/bin/sh
#
# Validate DKMS defaults, arbitrary explicit EtherLab source/build paths, and
# fail-closed handling for missing, wrong, and ambiguous inputs.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
kernel_release=${KERNEL_RELEASE:-$(uname -r)}
etherlab_include=${ETHERLAB_INCLUDE:-/usr/src/ethercat-dkms-1.6.9/include}
etherlab_symvers=${ETHERLAB_SYMVERS:-/var/lib/dkms/ethercat-dkms/1.6.9/"$kernel_release"/$(uname -m)/module/Module.symvers}
kernel_symvers=${KERNEL_SYMVERS:-/lib/modules/"$kernel_release"/build/Module.symvers}
tmp_dir=$(mktemp -d)

cleanup()
{
	rm -rf -- "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

require_failure()
{
	name=$1
	shift
	set +e
	"$@" >"$tmp_dir/$name.log" 2>&1
	status=$?
	set -e
	if [ "$status" -eq 0 ]; then
		echo "error: $name unexpectedly succeeded" >&2
		exit 1
	fi
}

test -f "$etherlab_include/ecrt.h"
test -f "$etherlab_symvers"
test -f "$kernel_symvers"

make -s -C "$project_dir" check-build-env

mkdir -p "$tmp_dir/source/include" "$tmp_dir/build"
cp "$etherlab_include/ecrt.h" "$tmp_dir/source/include/ecrt.h"
cp "$etherlab_symvers" "$tmp_dir/build/Module.symvers"

make -s -C "$project_dir" check-build-env \
	ETHERLAB_INCLUDE="$tmp_dir/source/include" \
	ETHERLAB_SYMVERS="$tmp_dir/build/Module.symvers"
make -s -C "$project_dir" modules \
	ETHERLAB_INCLUDE="$tmp_dir/source/include" \
	ETHERLAB_SYMVERS="$tmp_dir/build/Module.symvers"

modinfo "$project_dir/kernel/cw_ethercat.ko" |
	grep -q "^vermagic:[[:space:]]*$kernel_release "
modinfo "$project_dir/kernel/cw_ethercat.ko" |
	grep -q "^license:[[:space:]]*GPL$"

require_failure missing-header make -s -C "$project_dir" check-build-env \
	ETHERLAB_INCLUDE="$tmp_dir/missing" \
	ETHERLAB_SYMVERS="$tmp_dir/build/Module.symvers"
require_failure wrong-symbols make -s -C "$project_dir" check-build-env \
	ETHERLAB_INCLUDE="$tmp_dir/source/include" \
	ETHERLAB_SYMVERS="$kernel_symvers"
require_failure ambiguous-auto make -s -C "$project_dir" check-build-env \
	"ETHERLAB_DKMS_SOURCE_DIRS=$tmp_dir/ethercat-dkms-1 $tmp_dir/ethercat-dkms-2"

make -s -C "$project_dir" check-build-env \
	"ETHERLAB_DKMS_SOURCE_DIRS=$tmp_dir/ethercat-dkms-1 $tmp_dir/ethercat-dkms-2" \
	ETHERLAB_INCLUDE="$tmp_dir/source/include" \
	ETHERLAB_SYMVERS="$tmp_dir/build/Module.symvers"

echo "PASS: default and explicit EtherLab build paths validated; missing, wrong, and ambiguous inputs rejected"
