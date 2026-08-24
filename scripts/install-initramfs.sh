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

# TODO(joao):
#   1. validate that $WORKVM/install-app.sh exists; if not, tell the user to run
#        make starter
#      (do not copy anything from outside the repository — there is nothing
#      outside it)
#   2. validate that $APP exists, is x86-64 and is static
#        file "$APP" | grep -q 'statically linked'
#   3. "$WORKVM/install-app.sh" "$(readlink -f "$APP")"
#        (it already calls repack-initramfs.sh internally)
#   4. fail with a clear message on any error — this script runs inside make and
#      make MUST fail when something goes wrong
echo "TODO: implement install-initramfs.sh"
exit 1
