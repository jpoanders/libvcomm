# =============================================================================
# libvcomm — INE5424 Stage 1 — Group M10
#
# The assignment requires that `make` at the root compile AND run every
# evaluation test, and that the average latency be printed automatically at the
# end.
#
# What ALREADY WORKS:  app, test-support, test-stack, test-engine, starter,
#                      doctor, clean
# What is a SKELETON:  image, fleet, capture, stats  (look for TODO)
#
# The repository is self-contained: the instructor's starter is in vendor/ and
# no target here points outside the root.  `make doctor` says what is missing on
# the machine before you find out in the middle of the fleet run.
#
# Once fleet/capture/stats are done, change the .DEFAULT_GOAL line below to
# `check` — then a bare `make` runs the whole evaluation, as the assignment
# asks.
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
VM_TIMEOUT ?= 20

LIB_SRC  := src/raw_socket_engine.cpp
APP_SRC  := app/main.cpp $(LIB_SRC)
HEADERS  := $(wildcard include/*.h include/engine/*.h)

.DEFAULT_GOAL := all

# -----------------------------------------------------------------------------
.PHONY: all
all: app test-support
	@echo
	@echo "  build ok. Next step: 'make test-stack' and implement until it is green."
	@echo "  (image/fleet/capture/stats are still skeletons — see the TODOs in the Makefile)"

# The evaluation's final target: THIS is what the assignment wants `make` to do.
#
# VCOMM_REQUIRE_RAW=1 makes test-engine FAIL instead of skipping level 1 when
# there is no CAP_NET_RAW.  The evaluation target must not go green having
# skipped the only test that opens a real socket.  Running `make test-engine` on
# its own leaves the variable unset and the skip stays friendly.
.PHONY: check
check: export VCOMM_REQUIRE_RAW := 1
check: app test-support test-stack test-engine image fleet capture stats

# -----------------------------------------------------------------------------
.PHONY: app
app: $(BUILD)/student-app

$(BUILD)/student-app: $(APP_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) $(APPFLAGS) $(APP_SRC) -o $@ $(LDFLAGS)
	@file $@ | grep -q 'statically linked' || { echo "ERROR: binary is not static"; exit 1; }
	@file $@ | grep -q 'x86-64'            || { echo "ERROR: binary is not x86-64"; exit 1; }
	@echo "  ok: $@ is static x86-64"

# -----------------------------------------------------------------------------
.PHONY: test-support test-stack test-engine
test-support: $(BUILD)/test-support
	@echo
	./$(BUILD)/test-support

test-stack: $(BUILD)/test-stack
	@echo
	./$(BUILD)/test-stack

# Runs without privileges (level 1 skips itself).  To exercise the real socket,
# once:  sudo setcap cap_net_raw+ep $(BUILD)/test-engine
test-engine: $(BUILD)/test-engine
	@echo
	./$(BUILD)/test-engine

$(BUILD)/test-support: tests/test_support.cpp tests/check.h $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itests tests/test_support.cpp -o $@ $(LDFLAGS)

$(BUILD)/test-stack: tests/test_stack.cpp tests/check.h $(APP_SRC) $(HEADERS) | $(BUILD)
	$(CXX) $(CXXFLAGS) -Itests tests/test_stack.cpp $(LIB_SRC) -o $@ $(LDFLAGS)

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

.PHONY: image
image: app starter
	@echo "TODO(joao): inject the binary into the initramfs."
	@echo "  One binary for all 5 VMs; the role comes from SO2_VM_ID."
	@echo "  Steps: $(VMDIR)/install-app.sh \$$(readlink -f $(BUILD)/student-app)"
	@echo "  See scripts/install-initramfs.sh"
	@echo "  The 'starter' target already guaranteed the working copy — the"
	@echo "  vendor/ tarball is untouched, and make clean-vm undoes any damage."
	@false

# -----------------------------------------------------------------------------
.PHONY: fleet
fleet: image
	@echo "TODO(joao): bring up the 5 VMs in parallel, with a timeout and one log per VM."
	@echo "  See scripts/run-fleet.sh"
	@false

# -----------------------------------------------------------------------------
.PHONY: capture
capture:
	@echo "TODO(joao): capture the bus on the HOST while the fleet runs."
	@echo "  QEMU's mcast traffic goes through the host's loopback."
	@echo "  See scripts/capture.sh"
	@false

# -----------------------------------------------------------------------------
.PHONY: stats
stats:
	@echo "TODO(joao): extract the request/response pairs from the capture and"
	@echo "  compute count, mean, min, max and a percentile."
	@echo "  HONEST LABEL: say whether it is round-trip or a one-way estimate."
	@echo "  And say the bench runs in TCG (no /dev/kvm) — the number is biased."
	@echo "  See scripts/analyze-capture.sh"
	@false

# -----------------------------------------------------------------------------
$(BUILD):
	@mkdir -p $(BUILD)

.PHONY: clean
clean:
	rm -rf $(BUILD)/student-app $(BUILD)/test-support $(BUILD)/test-stack \
	       $(BUILD)/test-engine \
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
	@echo "== mandatory =="
	@for t in $(CXX) make file; do \
	    printf '  %-22s ' "$$t"; command -v $$t || { echo MISSING; exit 1; }; \
	done
	@echo "== fleet and measurement =="
	@for t in qemu-system-x86_64 cpio timeout dumpcap tshark; do \
	    printf '  %-22s ' "$$t"; command -v $$t || echo "MISSING (image/fleet/capture/stats)"; \
	done
	@printf '  %-22s ' setcap; command -v setcap || ls /usr/sbin/setcap 2>/dev/null || echo "MISSING (test-engine level 1)"
	@echo "== bench =="
	@test -e /dev/kvm && echo "  /dev/kvm               present" \
	    || echo "  /dev/kvm               ABSENT — QEMU in TCG, the latency is biased by emulation"
	@printf '  bus                    %s -> ' "$(SO2_MCAST)"; \
	    ip route get $(firstword $(subst :, ,$(SO2_MCAST))) 2>/dev/null | head -1 || echo "no route"
	@echo "     (if it does not say 'dev lo', a capture on lo comes back empty:"
	@echo "      sudo ip route add $(firstword $(subst :, ,$(SO2_MCAST)))/32 dev lo)"

.PHONY: help
help:
	@echo "make app           build the VM's static binary"
	@echo "make test-support  test the support classes (must pass today)"
	@echo "make test-stack    test the stack (Observer, pool, marshalling)"
	@echo "make test-engine   test the Engine (level 1 needs CAP_NET_RAW)"
	@echo "                   RX error proof: sudo scripts/test-engine-veth.sh"
	@echo "make starter       unpack vendor/ into build/vm/"
	@echo "make doctor        check the bench tools"
	@echo "make image         inject into the initramfs      [TODO]"
	@echo "make fleet         bring up the 5 VMs             [TODO]"
	@echo "make capture       capture the bus                [TODO]"
	@echo "make stats         compute the latency            [TODO]"
	@echo "make check         the whole evaluation           [TODO]"
	@echo "make clean         delete binaries and logs"
	@echo "make clean-vm      delete the VM working copy"
	@echo "make distclean     delete the whole build/"
