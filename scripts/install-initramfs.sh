#!/bin/bash

set -euo pipefail

WORKVM="${WORKVM:-build/vm}"
APP="${1:-build/student-app}"

die() { echo "install-initramfs: $*" >&2; exit 1; }

[ -x "$WORKVM/install-app.sh" ] ||
    die "no $WORKVM/install-app.sh — unpack the starter first:  make starter"

[ -f "$APP" ] || die "no $APP — build it first:  make app"

file "$APP" | grep -q 'x86-64' ||
    die "$APP is not an x86-64 executable"
file "$APP" | grep -q 'statically linked' ||
    die "$APP is not statically linked — the initramfs has no dynamic loader,
     so a dynamic binary fails inside the VM with a misleading 'not found'"

"$WORKVM/install-app.sh" "$(readlink -f "$APP")"
