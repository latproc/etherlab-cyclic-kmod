#!/bin/bash
# Rebuild and reinstall elc-ethercat via DKMS from this source tree, then
# reload the kernel modules.
#
# Typical use after pulling module changes:
#   sudo ./scripts/elc-dkms-reinstall.sh
#
# What it does:
#   1. make check-build-env + make dkms-install  (stage, build, install --force)
#   2. unload elc_ethercat / elc_ethercat_probe if loaded
#   3. depmod + modprobe elc_ethercat
#   4. print dkms status, modinfo version, and optional elc_bus API line
#
# Options:
#   --no-reload     install only; leave currently loaded modules alone
#   --keep-old      do not remove other elc-ethercat DKMS package versions
#   --check-only    verify build env + print status; do not install
#   -h, --help      this text
#
# Environment:
#   ELC_SRC                 source tree (default: parent of scripts/)
#   KERNEL_RELEASE          target kernel (default: uname -r)
#   ELC_CYCLE_CPU           module param if no /etc/modprobe.d entry (default: -1)
#   ELC_CYCLE_FIFO_PRIORITY same (default: 0 = leave scheduler policy)
#   ETHERLAB_INCLUDE / ETHERLAB_SYMVERS / ETHERLAB_VERSION
#                           same as make; use local.mk or export for non-DKMS EtherLab
#
# See docs/building/elc-dkms.md

set -euo pipefail

_SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ELC_SRC="${ELC_SRC:-$(cd "${_SCRIPT_DIR}/.." && pwd)}"
KERNEL_RELEASE="${KERNEL_RELEASE:-$(uname -r)}"
ELC_CYCLE_CPU="${ELC_CYCLE_CPU:--1}"
ELC_CYCLE_FIFO_PRIORITY="${ELC_CYCLE_FIFO_PRIORITY:-0}"

DO_RELOAD=1
KEEP_OLD=0
CHECK_ONLY=0

red() { printf '\033[31m%s\033[0m\n' "$*" >&2; }
grn() { printf '\033[32m%s\033[0m\n' "$*"; }
ylw() { printf '\033[33m%s\033[0m\n' "$*"; }

usage() {
	sed -n '2,30p' "$0" | sed 's/^# \?//'
}

need_root() {
	if [ "$(id -u)" -ne 0 ]; then
		red "ERROR: run as root (dkms install / module load)"
		exit 1
	fi
}

# LIB_VERSION from Makefile (e.g. 0.19.0).
lib_version() {
	local v
	v="$(cd "${ELC_SRC}" && make -s --no-print-directory -f Makefile \
		-f /dev/stdin <<<'print-ver:; @echo $(LIB_VERSION)' print-ver 2>/dev/null || true)"
	if [ -n "${v}" ]; then
		printf '%s\n' "${v}"
		return 0
	fi
	awk '
		/^LIB_VERSION_MAJOR[[:space:]]*:?=/ { maj=$3 }
		/^LIB_VERSION_MINOR[[:space:]]*:?=/ { min=$3 }
		END { if (maj != "" && min != "") print maj "." min ".0" }
	' "${ELC_SRC}/Makefile"
}

device_busy() {
	if [ -c /dev/elc_ethercat0 ] && command -v fuser >/dev/null 2>&1; then
		if fuser /dev/elc_ethercat0 >/dev/null 2>&1; then
			return 0
		fi
	fi
	return 1
}

unload_modules() {
	local m
	for m in elc_ethercat elc_ethercat_probe; do
		if grep -q "^${m} " /proc/modules 2>/dev/null; then
			echo "rmmod ${m}"
			if ! rmmod "${m}"; then
				red "ERROR: cannot unload ${m} (close control fds / stop controller)"
				if device_busy; then
					ylw "holders of /dev/elc_ethercat0:"
					fuser -v /dev/elc_ethercat0 2>&1 || true
				fi
				exit 1
			fi
		fi
	done
}

load_module() {
	local args=()
	# Prefer /etc/modprobe.d; only pass CLI options when non-default so
	# site conf is not overridden by script defaults.
	if [ "${ELC_CYCLE_CPU}" != "-1" ]; then
		args+=("cycle_cpu=${ELC_CYCLE_CPU}")
	fi
	if [ "${ELC_CYCLE_FIFO_PRIORITY}" != "0" ]; then
		args+=("cycle_fifo_priority=${ELC_CYCLE_FIFO_PRIORITY}")
	fi
	echo "modprobe elc_ethercat${args[*]:+ ${args[*]}}"
	modprobe elc_ethercat "${args[@]+"${args[@]}"}"
}

remove_old_versions() {
	local cur want line ver
	want="$(lib_version)"
	if [ -z "${want}" ]; then
		ylw "WARN: could not determine LIB_VERSION; skipping old-version cleanup"
		return 0
	fi
	while IFS= read -r line; do
		# elc-ethercat/0.18.0, ... or elc-ethercat/0.18.0: ...
		ver="$(printf '%s\n' "${line}" | sed -n 's|^elc-ethercat/\([^,: ]*\).*|\1|p')"
		[ -n "${ver}" ] || continue
		if [ "${ver}" = "${want}" ]; then
			continue
		fi
		echo "removing old DKMS package elc-ethercat/${ver}"
		dkms remove -m elc-ethercat -v "${ver}" --all || true
		rm -rf "/usr/src/elc-ethercat-${ver}"
	done < <(dkms status -m elc-ethercat 2>/dev/null || true)
}

print_status() {
	echo
	echo "=== DKMS / module status ==="
	if command -v dkms >/dev/null 2>&1; then
		dkms status -m elc-ethercat 2>/dev/null || ylw "(no elc-ethercat dkms lines)"
	fi
	if modinfo elc_ethercat >/dev/null 2>&1; then
		modinfo elc_ethercat | grep -E '^(filename|version|vermagic|description):' || true
	else
		ylw "modinfo elc_ethercat: not found"
	fi
	if [ -d /sys/module/elc_ethercat ]; then
		grn "loaded: elc_ethercat"
		if [ -x "${ELC_SRC}/tools/elc_bus" ]; then
			# One open/close: claims master briefly; fine for plant reinstall verify.
			"${ELC_SRC}/tools/elc_bus" 2>/dev/null | head -n 3 || true
		fi
	else
		ylw "module not loaded"
	fi
}

while [ $# -gt 0 ]; do
	case "$1" in
	--no-reload) DO_RELOAD=0 ;;
	--keep-old) KEEP_OLD=1 ;;
	--check-only) CHECK_ONLY=1 ;;
	-h | --help)
		usage
		exit 0
		;;
	*)
		red "unknown option: $1"
		usage
		exit 2
		;;
	esac
	shift
done

if [ ! -f "${ELC_SRC}/Makefile" ] || [ ! -f "${ELC_SRC}/dkms.conf.in" ]; then
	red "ERROR: ELC_SRC does not look like this tree: ${ELC_SRC}"
	exit 1
fi

if ! command -v dkms >/dev/null 2>&1; then
	red "ERROR: dkms is not installed"
	exit 1
fi

echo "ELC_SRC=${ELC_SRC}"
echo "KERNEL_RELEASE=${KERNEL_RELEASE}"
echo "package version (LIB_VERSION)=$(lib_version)"

cd "${ELC_SRC}"

if [ "${CHECK_ONLY}" -eq 1 ]; then
	make check-build-env KERNEL_RELEASE="${KERNEL_RELEASE}"
	print_status
	exit 0
fi

need_root

echo
echo "=== check build env ==="
make check-build-env KERNEL_RELEASE="${KERNEL_RELEASE}"

echo
echo "=== make dkms-install ==="
make dkms-install KERNEL_RELEASE="${KERNEL_RELEASE}"

if [ "${KEEP_OLD}" -eq 0 ]; then
	echo
	echo "=== remove older elc-ethercat DKMS versions ==="
	remove_old_versions
fi

if [ "${DO_RELOAD}" -eq 1 ]; then
	echo
	echo "=== reload modules ==="
	if device_busy; then
		red "ERROR: /dev/elc_ethercat0 is open; stop the controller before reload"
		fuser -v /dev/elc_ethercat0 2>&1 || true
		exit 1
	fi
	unload_modules
	depmod -a "${KERNEL_RELEASE}"
	load_module
else
	ylw "skipped reload (--no-reload); unload/modprobe when ready"
fi

print_status

want="$(lib_version)"
got="$(modinfo -F version elc_ethercat 2>/dev/null || true)"
if [ -n "${want}" ] && [ -n "${got}" ] && [ "${got}" != "${want}" ]; then
	red "ERROR: installed module version ${got} != expected ${want}"
	exit 1
fi
if [ "${DO_RELOAD}" -eq 1 ] && ! grep -q '^elc_ethercat ' /proc/modules; then
	red "ERROR: elc_ethercat failed to load"
	exit 1
fi

grn "DKMS reinstall complete (elc-ethercat/${want:-?})"
