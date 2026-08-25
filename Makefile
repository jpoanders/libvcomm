# =============================================================================
# libvcomm — INE5424 Stage 1 — Group M10
#
# The assignment requires that `make` at the root compile AND run every
# evaluation test, and that the average latency be printed automatically at the
# end.
#
# A bare `make` runs the WHOLE evaluation:
#
#     compile -> host tests -> inject into the initramfs -> boot the fleet
#             -> capture the bus -> prove the frame layout -> print the latency
#
# and it FAILS, with a non-zero status, if a receiver loses a frame, a VM blows
# its timeout, the capture comes back empty, or the statistics cannot be
# computed.  A target that only prints warnings is not gradeable.
#
# NOTHING IN THIS PIPELINE NEEDS sudo, AND NOTHING IN IT WILL EVER PROMPT FOR A
# PASSWORD.  That is a hard requirement, not a convenience: an evaluator who is
# asked for a password before the build will reasonably conclude the delivery
# does not build.  The two places that classically want privileges are handled
# instead of demanded --
#
#   capturing the bus   build/bus-tap joins QEMU's multicast group with an
#                       ordinary UDP socket, so the latency below is computed
#                       without dumpcap, without tshark and without membership
#                       of the `wireshark` group (dumpcap is installed 0754
#                       root:wireshark on Debian and Ubuntu: a grader outside
#                       that group cannot even execute it);
#   test-engine level 1 scripts/run-engine-test.sh climbs a ladder of ways to
#                       get CAP_NET_RAW that cannot prompt, and if every rung
#                       fails it says so and continues -- the fleet below then
#                       exercises the same raw sockets inside the VMs anyway.
#
# `make caps` and `make bus-local` exist for the two optional extras that do
# want sudo.  Neither is part of `make`.
#
# The repository is self-contained: the instructor's starter is in vendor/ and
# no target here points outside the root.  `make doctor` says what is missing on
# the machine before you find out in the middle of the fleet run.
# =============================================================================

CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -O2 -Iinclude
LDFLAGS  := -pthread

# The binary that goes inside the VM: it MUST be static x86_64, otherwise
# install-app.sh refuses it (the starter's initramfs has no dynamic loader).
APPFLAGS := -static

BUILD   := build

# The instructor's starter lives in vendor/, versioned along with the
# repository.  Nothing here points outside the root: a clean clone on any
# machine runs the whole make.  See vendor/README.md for why, and for the price.
STARTER_TGZ := vendor/INE5424-x86_64-starter-6.15.5.tar.gz
STARTER_SUM := $(STARTER_TGZ).sha256
STARTER_DIR := INE5424-x86_64-starter-6.15.5

# The working copy.  repack-initramfs.sh writes into the tree it lives in, so it
# ALWAYS runs here, never in the tarball.  make clean-vm restores the image to
# factory state.
VMDIR   := $(BUILD)/vm
VMSTAMP := $(VMDIR)/.unpacked

# The group's virtual bus.  run-vm.sh's default (230.0.0.1:1234) collides with
# other groups on the same machine — keep an address of our own.
SO2_MCAST ?= 239.10.10.10:15424
export SO2_MCAST

VMS      := 1 2 3 4 5
VM_TIMEOUT ?= 60   # ceiling only: the app powers its vehicle off when done

LIB_SRC  := src/raw_socket_engine.cpp
APP_SRC  := app/main.cpp $(LIB_SRC)
TAP_SRC  := tools/bus_tap.cpp
HEADERS  := $(wildcard include/*.h include/engine/*.h app/*.h)

# setcap/getcap live in /usr/sbin, which is not on a normal user's PATH.
SETCAP := $(shell command -v setcap 2>/dev/null || echo /usr/sbin/setcap)
GETCAP := $(shell command -v getcap 2>/dev/null || echo /usr/sbin/getcap)

# The evaluation is a pipeline: the fleet has to run before there is a capture
# to prove, and the capture before there is a latency to compute.  A grader
# typing `make -j8` must not get those in some other order.
.NOTPARALLEL:

# THE ASSIGNMENT'S REQUIREMENT, in one line: `make` at the root compiles and
# runs every evaluation test.
.DEFAULT_GOAL := check

# -----------------------------------------------------------------------------
.PHONY: all
all: app tap test-support test-stack test-protocol
	@echo
	@echo "  build and host tests ok. 'make check' (or just 'make') runs the fleet too."

# The evaluation's final target: THIS is what the assignment wants `make` to do.
# It runs to the end on a bare machine, with no privileges and no interaction.
.PHONY: check
check: app tap test-support test-stack test-protocol test-engine \
       image fleet capture stats
	@echo
	@echo "  =========================================================="
	@echo "   make check: everything green."
	@echo "  =========================================================="

# OPTIONAL, and deliberately not a prerequisite of anything.  Grants
# build/test-engine the capability its level 1 wants, so the raw socket is
# exercised on the host as well as inside the VMs.  `make` is complete without
# it; this only moves one proof earlier.  The capability lives on the inode, so
# it has to be redone after every relink.
.PHONY: caps
caps: $(BUILD)/test-engine
	sudo $(SETCAP) cap_net_raw+ep $(BUILD)/test-engine
	@echo "  ok: cap_net_raw+ep on $(BUILD)/test-engine"
	@echo "  (optional: run-engine-test.sh already handles its absence)"

# -----------------------------------------------------------------------------
.PHONY: app
app: $(BUILD)/student-app

$(BUILD)/student-app: $(APP_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(APPFLAGS) $(APP_SRC) -o $@ $(LDFLAGS)
	@file $@ | grep -q 'statically linked' || { echo "ERROR: binary is not static"; exit 1; }
	@file $@ | grep -q 'x86-64'            || { echo "ERROR: binary is not x86-64"; exit 1; }
	@echo "  ok: $@ is static x86-64"

# The host-side recorder for the bus.  Not static and never enters the
# initramfs: it runs on the host, beside QEMU, not inside a vehicle.
.PHONY: tap
tap: $(BUILD)/bus-tap

$(BUILD)/bus-tap: $(TAP_SRC) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(TAP_SRC) -o $@
	@echo "  ok: $@ (records the bus with no privileges)"

# -----------------------------------------------------------------------------
.PHONY: test-support test-stack test-protocol test-engine
test-support: $(BUILD)/test-support
	@echo
	./$(BUILD)/test-support

test-stack: $(BUILD)/test-stack
	@echo
	./$(BUILD)/test-stack

# Protocol and Communicator over tests/loopback_engine.h: no socket, no
# privileges, no VM.  It is what catches an addressing or ownership bug on the
# host in milliseconds instead of through a two-minute fleet run.
test-protocol: $(BUILD)/test-protocol
	@echo
	./$(BUILD)/test-protocol

# The wrapper gets as much privilege as it can WITHOUT PROMPTING, runs level 1
# if it succeeded, and says which rung it reached either way.  See the ladder
# documented at the top of scripts/run-engine-test.sh.
test-engine: $(BUILD)/test-engine
	@echo
	SETCAP=$(SETCAP) GETCAP=$(GETCAP) scripts/run-engine-test.sh

$(BUILD)/test-support: tests/test_support.cpp tests/check.h $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itests tests/test_support.cpp -o $@ $(LDFLAGS)

$(BUILD)/test-stack: tests/test_stack.cpp tests/check.h $(APP_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itests tests/test_stack.cpp $(LIB_SRC) -o $@ $(LDFLAGS)

$(BUILD)/test-protocol: tests/test_protocol.cpp tests/check.h tests/loopback_engine.h $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itests tests/test_protocol.cpp -o $@ $(LDFLAGS)

$(BUILD)/test-engine: tests/test_engine.cpp tests/check.h $(LIB_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itests tests/test_engine.cpp $(LIB_SRC) -o $@ $(LDFLAGS)

# -----------------------------------------------------------------------------
# Unpacks the starter into $(VMDIR), checking the sha256 first.  The stamp
# exists so make does not unpack 15 MB on every invocation; it redoes itself if
# the tarball changes or if you run clean-vm.
.PHONY: starter
starter: $(VMSTAMP)

$(VMSTAMP): $(STARTER_TGZ) $(STARTER_SUM) | $(BUILD)
	@echo "  checking $(STARTER_TGZ)"
	@cd vendor && sha256sum -c $(notdir $(STARTER_SUM)) >/dev/null
	@rm -rf $(VMDIR)
	@mkdir -p $(VMDIR)
	@tar -xzf $(STARTER_TGZ) -C $(VMDIR) --strip-components=1 $(STARTER_DIR)
	@touch $@
	@echo "  ok: starter unpacked into $(VMDIR)/ (working copy)"

# One binary for all the VMs; the role comes from SO2_VM_ID.  The 'starter'
# target already guaranteed the working copy, so the vendor/ tarball is never
# touched and make clean-vm undoes any damage.
.PHONY: image
image: app starter
	@echo
	WORKVM=$(VMDIR) scripts/install-initramfs.sh $(BUILD)/student-app

# -----------------------------------------------------------------------------
# Boots the vehicles in parallel, captures the bus on the host while they run,
# and checks every vehicle's verdict.  Non-zero if any of them is missing.
.PHONY: fleet
fleet: image tap
	@echo
	WORKVM=$(VMDIR) VMS="$(VMS)" VM_TIMEOUT=$(VM_TIMEOUT) scripts/run-fleet.sh

# -----------------------------------------------------------------------------
# The capture's own verdict: proves the frame layout from the bytes on the wire,
# without reading a single VM log.  It is deliberately independent of `fleet`
# so it can be re-run against the last capture; run `make fleet` first.
.PHONY: capture
capture:
	@echo
	scripts/verify-capture.sh

# -----------------------------------------------------------------------------
.PHONY: stats
stats:
	@echo
	scripts/analyze-capture.sh

# -----------------------------------------------------------------------------
$(BUILD):
	@mkdir -p $(BUILD)

.PHONY: clean
clean:
	rm -rf $(BUILD)/student-app $(BUILD)/test-support $(BUILD)/test-stack \
	       $(BUILD)/test-protocol $(BUILD)/test-engine $(BUILD)/bus-tap \
	       $(BUILD)/logs $(BUILD)/captures

# Separate from clean because unpacking again costs 15 MB of I/O and clean runs
# all the time.  Use it when the image is dirty.
.PHONY: clean-vm
clean-vm:
	rm -rf $(VMDIR)

.PHONY: distclean
distclean: clean clean-vm
	rm -rf $(BUILD)

# Checks the tools before you discover one is missing in the middle of a fleet
# run.  Only the compiler is mandatory for the host tests; the rest is the
# bench.
.PHONY: doctor
doctor:
	@echo "== mandatory for the host tests =="
	@for t in $(CXX) make file; do \
	    printf '  %-22s ' "$$t"; command -v $$t || { echo MISSING; exit 1; }; \
	done
	@echo "== mandatory for the fleet =="
	@for t in qemu-system-x86_64 cpio timeout; do \
	    printf '  %-22s ' "$$t"; command -v $$t || echo "MISSING (image/fleet)"; \
	done
	@echo "== optional — 'make' is complete without every line below =="
	@printf '  %-22s ' dumpcap; command -v dumpcap \
	    || echo "absent (secondary recorder only; build/bus-tap is the one make uses)"
	@printf '  %-22s ' tshark;  command -v tshark \
	    || echo "absent (only needed to read a dumpcap .pcapng)"
	@printf '  %-22s ' setcap;  command -v setcap || ls /usr/sbin/setcap 2>/dev/null \
	    || echo "absent (test-engine level 1 runs in the VMs instead)"
	@echo "== privileges =="
	@echo "  none required. 'make' never calls sudo and never prompts."
	@printf '  %-22s ' "test-engine level 1"; \
	    ($(GETCAP) $(BUILD)/test-engine 2>/dev/null | grep -q cap_net_raw \
	      && echo "on the host (file capability present)") \
	    || (unshare -Urn true 2>/dev/null && echo "on the host (user namespace available)") \
	    || echo "inside the VMs only — see 'make caps' to add it here too"
	@echo "== bench =="
	@test -e /dev/kvm && echo "  /dev/kvm               present" \
	    || echo "  /dev/kvm               ABSENT — QEMU in TCG, the latency is biased by emulation"
	@printf '  %-22s %s\n' "bus" "$(SO2_MCAST) pinned to 127.0.0.1 by QEMU localaddr="
	@echo "     (the tap joins the group on the same address, so no route is needed;"
	@echo "      'make bus-local' is the fallback for a QEMU too old for localaddr)"

.PHONY: help
help:
	@echo "make               == make check, the whole evaluation"
	@echo "make check         compile, test, boot the fleet, capture, measure"
	@echo
	@echo "make app           build the VM's static binary"
	@echo "make all           build + the host tests only (no VMs)"
	@echo "make test-support  test the support classes"
	@echo "make test-stack    test the stack (Observer, pool, marshalling)"
	@echo "make test-protocol test Protocol + Communicator over a loopback Engine"
	@echo "make test-engine   test the Engine (level 1 uses whatever privilege it can get)"
	@echo "                   RX error proof: sudo scripts/test-engine-veth.sh"
	@echo "make tap           build the unprivileged bus recorder"
	@echo "make caps          OPTIONAL: grant test-engine CAP_NET_RAW on the host"
	@echo "                   (needs sudo, once per relink; make is complete without it)"
	@echo "make starter       unpack vendor/ into build/vm/"
	@echo "make doctor        check the bench tools"
	@echo "make image         inject the binary into the initramfs"
	@echo "make fleet         boot the vehicles and check their verdicts"
	@echo "make capture       prove the frame layout from the last capture"
	@echo "make stats         compute the latency from the last capture"
	@echo "make bus-local     fallback for a QEMU too old for localaddr= (needs sudo)"
	@echo "make clean         delete binaries and logs"
	@echo "make clean-vm      delete the VM working copy"
	@echo "make distclean     delete the whole build/"
	@echo
	@echo "useful variables:"
	@echo "  VMS=\"1 2\"       a smaller fleet while debugging"
	@echo "  VM_TIMEOUT=60    per-VM ceiling"
	@echo "  SO2_MCAST=...    the group's bus"

# The fleet pins the bus to loopback by passing QEMU localaddr=127.0.0.1, which
# needs no privileges.  This target is the fallback for a QEMU too old for that
# option: it does the same thing with a route, and needs sudo.
.PHONY: bus-local
bus-local:
	sudo ip route add $(firstword $(subst :, ,$(SO2_MCAST)))/32 dev lo
	@echo "  ok: $(SO2_MCAST) pinned to lo (disappears on reboot)"
