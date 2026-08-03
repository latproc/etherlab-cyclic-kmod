#!/bin/sh
#
# Hardware test: setup-hold begin / mailbox under hold / release, timeout
# force-release, and client-death auto-release. Zero-output only.
#
# Requires motion inhibited and root. Reloads elc_ethercat.

set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
module_path=${ELC_MODULE:-"$project_dir/kernel/elc_ethercat.ko"}
module_name=elc_ethercat
config=${ELC_CONFIG:-"$project_dir/tools/configs/el2034_core_console_pos15.conf"}
position=${ELC_HOLD_POSITION:-15}
period=${ELC_TEST_PERIOD_NS:-1000000}

if [ "${ELC_MOTION_INHIBITED:-}" != YES ]; then
	echo "error: set ELC_MOTION_INHIBITED=YES only after motion is safely inhibited" >&2
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

tmp_dir=$(mktemp -d)
controller_pid=
cleanup()
{
	if [ -n "$controller_pid" ] && kill -0 "$controller_pid" 2>/dev/null; then
		kill -KILL "$controller_pid" 2>/dev/null || true
		wait "$controller_pid" 2>/dev/null || true
	fi
	if grep -q "^$module_name " /proc/modules; then
		rmmod "$module_name" 2>/dev/null || true
	fi
	rm -rf -- "$tmp_dir"
}
trap cleanup EXIT HUP INT TERM

if grep -q "^$module_name " /proc/modules; then
	rmmod "$module_name"
fi

"$project_dir/tools/elc_capture_topology.sh" >"$tmp_dir/slaves-before.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-before.txt"
insmod "$module_path"

echo "=== 1) hold / apply-window / release ==="
"$project_dir/tools/elc_config" setup-hold \
	"$config" "$period" "$position" | tee "$tmp_dir/hold.txt"
grep -q 'PASS: setup-hold mode=0' "$tmp_dir/hold.txt"

echo "=== 2) timeout force-release ==="
"$project_dir/tools/elc_config" setup-hold-timeout \
	"$config" "$period" "$position" 2000 | tee "$tmp_dir/timeout.txt"
grep -q 'PASS: setup-hold mode=1' "$tmp_dir/timeout.txt"

echo "=== 3) client death releases hold ==="
stdbuf -oL -eL "$project_dir/tools/elc_config" setup-hold-death \
	"$config" "$period" "$position" 60000 >"$tmp_dir/death.txt" 2>&1 &
controller_pid=$!
ready=0
i=0
while [ "$i" -lt 200 ]; do
	if grep -q '^READY: setup-hold active' "$tmp_dir/death.txt"; then
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
	echo "error: death harness did not become READY" >&2
	sed 's/^/  /' "$tmp_dir/death.txt" >&2
	exit 1
fi
kill -KILL "$controller_pid"
set +e
wait "$controller_pid"
set -e
controller_pid=
ethercat master >"$tmp_dir/master-after-death.txt"
if ! grep -q 'Phase: Idle' "$tmp_dir/master-after-death.txt" ||
    ! grep -q 'Active: no' "$tmp_dir/master-after-death.txt"; then
	echo "error: master not idle after client death" >&2
	exit 1
fi
echo "client death returned master idle"

rmmod "$module_name"
"$project_dir/tools/elc_capture_topology.sh" >"$tmp_dir/slaves-after.txt"
cmp "$tmp_dir/slaves-before.txt" "$tmp_dir/slaves-after.txt"
dmesg --level=err,warn >"$tmp_dir/dmesg-after.txt"
before_lines=$(wc -l <"$tmp_dir/dmesg-before.txt")
sed -n "$((before_lines + 1)),\$p" "$tmp_dir/dmesg-after.txt" \
	>"$tmp_dir/dmesg-new.txt"
if grep -E 'BUG:|Oops:|general protection fault|use-after-free|KASAN:|kernel NULL pointer|hung task' \
	"$tmp_dir/dmesg-new.txt"; then
	echo "error: fatal kernel diagnostic during setup-hold tests" >&2
	exit 1
fi
echo "New kernel warning/error lines:"
if [ -s "$tmp_dir/dmesg-new.txt" ]; then
	sed 's/^/  /' "$tmp_dir/dmesg-new.txt"
else
	echo "  none"
fi
echo "PASS: setup-hold hold/release, timeout, and client-death"
