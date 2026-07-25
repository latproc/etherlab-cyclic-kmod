#!/bin/sh
#
# Emit only stable physical-order and identity fields from EtherLab discovery.
# AL state, DC receive timestamps, and other live diagnostics are intentionally
# excluded so lifecycle tests do not mistake transient state for remapping.

set -eu

tmp_file=$(mktemp)
cleanup()
{
	rm -f -- "$tmp_file"
}
trap cleanup EXIT HUP INT TERM

ethercat slaves -v >"$tmp_file"
awk '
/^=== Master [0-9]+, Slave [0-9]+ ===$/ {
	slave = $0
}
/^Identity:$/ {
	if ((getline vendor) <= 0 ||
	    (getline product) <= 0 ||
	    (getline revision) <= 0 ||
	    (getline serial) <= 0)
		exit 2
	print slave
	print vendor
	print product
	print revision
	print serial
	count++
}
END {
	if (!count)
		exit 3
}
' "$tmp_file"
