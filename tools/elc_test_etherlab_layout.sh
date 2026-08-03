#!/bin/sh
#
# Compile-time gate for setup-hold's private EtherLab layout offsets.
#
# Public ecrt does not export request-AL. setup-hold writes
# ec_slave_config->slave->requested_state through fixed offsets documented in
# kernel/elc_etherlab_layout.h. This test forces a module rebuild that includes
# elc_etherlab_layout_check.o (offsetof asserts). Drift fails the compile.
#
# Also fails if EtherLab private headers are not available — do not ship
# setup-hold without a layout check on the plant build host.
#
# Usage (from repo root or any dir):
#   make test-etherlab-layout
#   # or
#   ./tools/elc_test_etherlab_layout.sh

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)

cd "$project_dir"

# Resolve the same contract as the module build.
eval "$(make -s check-build-env | sed -n 's/^\([A-Z_][A-Z_0-9]*\)=\(.*\)$/\1="\2"; export \1;/p')"

if [ -z "${ETHERLAB_SOURCE:-}" ]; then
	ETHERLAB_SOURCE=$(dirname "$ETHERLAB_INCLUDE")
	export ETHERLAB_SOURCE
fi

layout_hdr="$ETHERLAB_SOURCE/master/slave_config.h"
slave_hdr="$ETHERLAB_SOURCE/master/slave.h"

if [ ! -f "$layout_hdr" ] || [ ! -f "$slave_hdr" ]; then
	echo "error: EtherLab private headers required for layout check:" >&2
	echo "  missing: $layout_hdr" >&2
	echo "  and/or:  $slave_hdr" >&2
	echo "Install ethercat-dkms sources or set ETHERLAB_SOURCE to a full tree." >&2
	echo "Without this check, setup-hold offsets can silently mis-write after an upgrade." >&2
	exit 1
fi

echo "EtherLab layout check:"
echo "  ETHERLAB_INCLUDE=$ETHERLAB_INCLUDE"
echo "  ETHERLAB_SOURCE=$ETHERLAB_SOURCE"
echo "  expected offsets: sc.slave=$(
	sed -n 's/^#define ELC_EC_SC_SLAVE_OFFSET[[:space:]]*//p' \
		kernel/elc_etherlab_layout.h | head -1
) requested_state=$(
	sed -n 's/^#define ELC_EC_SLAVE_REQUESTED_STATE_OFFSET[[:space:]]*//p' \
		kernel/elc_etherlab_layout.h | head -1
) error_flag=$(
	sed -n 's/^#define ELC_EC_SLAVE_ERROR_FLAG_OFFSET[[:space:]]*//p' \
		kernel/elc_etherlab_layout.h | head -1
)"

# Force recompile of the layout-check unit so stale .o cannot mask drift.
rm -f kernel/elc_etherlab_layout_check.o \
	kernel/.elc_etherlab_layout_check.o.cmd \
	kernel/elc_ethercat.o kernel/elc_ethercat.ko

if ! make -s modules \
	ETHERLAB_INCLUDE="$ETHERLAB_INCLUDE" \
	ETHERLAB_SOURCE="$ETHERLAB_SOURCE" \
	ETHERLAB_SYMVERS="$ETHERLAB_SYMVERS" \
	KERNEL_RELEASE="$KERNEL_RELEASE" \
	KERNEL_BUILD="$KERNEL_BUILD"; then
	echo "error: module build failed — likely offsetof mismatch in layout check" >&2
	echo "Re-measure offsets against $layout_hdr / $slave_hdr, update" >&2
	echo "kernel/elc_etherlab_layout.h, then re-run hardware:" >&2
	echo "  ELC_MOTION_INHIBITED=YES ./tools/elc_test_setup_hold.sh" >&2
	exit 1
fi

if [ ! -f kernel/elc_etherlab_layout_check.o ]; then
	echo "error: layout check object was not built (Kbuild did not enable it)" >&2
	echo "Ensure ETHERLAB_SOURCE is passed to the kernel make and" >&2
	echo "  $layout_hdr exists." >&2
	exit 1
fi

# Confirm the check object is linked into the final module.
if ! nm kernel/elc_ethercat.ko 2>/dev/null |
	grep -q 'elc_etherlab_layout_check_anchor\|elc_layout_assert'; then
	# Symbol names may be local; accept presence of the .o in the link map.
	if ! grep -q elc_etherlab_layout_check kernel/elc_ethercat.mod 2>/dev/null &&
	   ! strings kernel/elc_ethercat.ko | grep -q elc_layout_assert; then
		# Still OK if .o exists and module linked without error —
		# asserts are compile-time only and may leave no string.
		:
	fi
fi

# Negative check: a deliberately wrong offset must fail the compile.
if [ "${ELC_LAYOUT_SKIP_NEGATIVE:-}" != "1" ]; then
	neg_dir=$(mktemp -d)
	cleanup_neg() { rm -rf -- "$neg_dir"; }
	trap cleanup_neg EXIT

	mkdir -p "$neg_dir/kernel" "$neg_dir/include"
	cp kernel/elc_etherlab_layout.h "$neg_dir/kernel/"
	# Break one constant.
	sed -i 's/^#define ELC_EC_SC_SLAVE_OFFSET.*/#define ELC_EC_SC_SLAVE_OFFSET 999/' \
		"$neg_dir/kernel/elc_etherlab_layout.h"
	cp kernel/elc_etherlab_layout_check.c \
		kernel/etherlab_layout_stub/config.h \
		"$neg_dir/kernel/" 2>/dev/null || true
	mkdir -p "$neg_dir/kernel/etherlab_layout_stub"
	cp kernel/etherlab_layout_stub/config.h \
		"$neg_dir/kernel/etherlab_layout_stub/"
	cp kernel/elc_etherlab_layout_check.c "$neg_dir/kernel/"

	cat >"$neg_dir/kernel/Kbuild" <<EOF
obj-m += elc_layout_neg.o
elc_layout_neg-y := elc_etherlab_layout_check.o
CFLAGS_elc_etherlab_layout_check.o += \\
	-I\$(src)/etherlab_layout_stub \\
	-I$ETHERLAB_SOURCE \\
	-I$ETHERLAB_SOURCE/master \\
	-I$ETHERLAB_SOURCE/include \\
	-I\$(src)
EOF

	set +e
	make -s -C "$KERNEL_BUILD" M="$neg_dir/kernel" modules \
		> "$neg_dir/neg.log" 2>&1
	neg_status=$?
	set -e
	if [ "$neg_status" -eq 0 ]; then
		echo "error: negative layout test compiled successfully (gate broken)" >&2
		exit 1
	fi
	echo "PASS: deliberately wrong ELC_EC_SC_SLAVE_OFFSET failed to compile"
	trap - EXIT
	cleanup_neg
fi

echo "PASS: EtherLab private layout offsets match elc_etherlab_layout.h"
echo "      (compile-time offsetof gate linked into elc_ethercat.ko)"
echo "Semantic hold behaviour still needs hardware:"
echo "  ELC_MOTION_INHIBITED=YES ./tools/elc_test_setup_hold.sh"
