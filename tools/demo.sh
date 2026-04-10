#!/bin/sh
set -eu

ROOT=$(cd "$(dirname "$0")/.." && pwd)
FWSIM=${1:-$ROOT/build/fwsim}
case $FWSIM in /*) ;; *) FWSIM=$(pwd)/$FWSIM ;; esac
WORK=$(mktemp -d "${TMPDIR:-/tmp}/fwsim-demo.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

run() {
    echo "\$ $*"
    "$@"
    echo
}

run_fails() {
    echo "\$ $*"
    if "$@"; then
        echo "demo: expected '$*' to fail" >&2
        exit 1
    fi
    echo
}

fwsim() { "$FWSIM" "$@"; }
mkimage() { python3 "$ROOT/tools/mkimage.py" "$@"; }

echo "# Build three deterministic images."
run mkimage --fw-version 0x00010000 --size 8192  --seed 1 -o v1.img
run mkimage --fw-version 0x00010100 --size 12000 --seed 2 -o v2.img
run mkimage --fw-version 0x00010200 --size 9000  --seed 3 -o v3.img

echo "# 1. Factory state: v1 confirmed in slot A."
run fwsim init v1.img
run fwsim boot

echo "# 2. Successful update: stage v2, trial-boot it, confirm it."
run fwsim install v2.img
run fwsim status
run fwsim boot
run fwsim confirm
run fwsim boot

echo "# 3. Rollback: stage v3, trial-boot it, then reboot without confirming."
run fwsim install v3.img
run fwsim boot
run fwsim boot
run fwsim status

echo "# 4. A truncated image is rejected; the confirmed image keeps booting."
head -c 5000 v3.img > v3-truncated.img
run_fails fwsim install v3-truncated.img
run fwsim boot
