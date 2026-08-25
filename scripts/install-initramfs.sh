#!/bin/bash
# Injects build/student-app into the initramfs and leaves the image ready for
# the fleet.
#
# The working copy comes ready: `make starter` checks the sha256 of the tarball
# in vendor/ and unpacks it into build/vm/.  repack-initramfs.sh writes into the
# tree it lives in, which is why it runs in build/vm/ and never in vendor/ — if
# the image gets dirty, `make clean-vm` restores it to factory state.
set -euo pipefail

WORKVM="${WORKVM:-build/vm}"
APP="${1:-build/student-app}"

die() { echo "install-initramfs: $*" >&2; exit 1; }

[ -x "$WORKVM/install-app.sh" ] ||
    die "no $WORKVM/install-app.sh — unpack the starter first:  make starter"

[ -f "$APP" ] || die "no $APP — build it first:  make app"

# The two checks install-app.sh makes anyway.  Repeating them here is not
# redundant: this script is what `make image` calls, and a failure phrased in
# the project's own terms beats one phrased in the starter's.
file "$APP" | grep -q 'x86-64' ||
    die "$APP is not an x86-64 executable"
file "$APP" | grep -q 'statically linked' ||
    die "$APP is not statically linked — the initramfs has no dynamic loader,
     so a dynamic binary fails inside the VM with a misleading 'not found'"

"$WORKVM/install-app.sh" "$(readlink -f "$APP")"
