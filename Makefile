###############################################################################
#
# Makefile for LXR firmware image
# Author: Patrick Dowling
#
# This makefile uses the subdirectory makefiles to build
# - cortex firmware image
# - AVR firmware image
# - FirmwareImageBuilder tool
# - Binary firmware image
#
# For the cross-platform builds, the necessary compilers (ARM, AVR) must either
# be in the path, or the installation directories can be specified using
#  ARM_TOOLKIT_ROOT
# and
#  AVR_TOOLKIT_ROOT
#
# TODO(pld) Additional build options can be specified using environment variables
#  DESTDIR : where final binary image is written
#  FIRMWARE : name of final binary image
#
###############################################################################

ifdef VERBOSE
AT :=
else
AT := @
endif

ARM_PATH=./mainboard/LxrStm32/
AVR_PATH=./front/LxrAvr/
BINPATH=./tools/bin/
DESTDIR?=./firmware\ image/

ARM_BINARY=$(ARM_PATH)LxrStm32.bin
AVR_BINARY=$(AVR_PATH)LxrAvr.bin
# The image builder is a host executable. Keep one generated copy per host
# platform so an x86_64 prebuilt cannot be selected on an arm64 machine.
HOST_OS?=$(shell uname -s 2>/dev/null || echo unknown)
HOST_ARCH?=$(shell uname -m 2>/dev/null || echo unknown)
FIB?=$(BINPATH)FirmwareImageBuilder_$(HOST_OS)_$(HOST_ARCH)
FIRMWARE?=FIRMWARE.BIN
IMAGE?=$(DESTDIR)$(FIRMWARE)
JOBS?=-j3

.PHONY: all
all:
	@echo "Valid targets are:"
	@echo " clean : clean subdirectories"
	@echo " builder : build the host-native firmware image builder"
	@echo " firmware : build firmware"

.PHONY: clean
clean:
	$(AT)make -C tools/FirmwareImageBuilder clean
	$(AT)$(RM) $(FIB)
	$(AT)make -C $(AVR_PATH) clean
	$(AT)make -C $(ARM_PATH) clean
	$(AT)$(RM) $(IMAGE)

.PHONY: firmware
firmware: $(IMAGE)

.PHONY: builder
builder: $(FIB)

$(IMAGE): $(ARM_BINARY) $(AVR_BINARY) $(FIB)
	@echo "Building final firmware image $@..."
	$(AT)$(FIB) $(ARM_BINARY) $(AVR_BINARY) "$@"

$(FIB): tools/FirmwareImageBuilder/FirmwareImageBuilder/FirmwareImageBuilder.cpp
	@echo "** Building $@..."
	$(AT)$(MAKE) -C tools/FirmwareImageBuilder EXE=../bin/$(notdir $@) exe

$(AVR_BINARY):
	@echo "** Building $@..."
	$(AT)make $(JOBS) -C $(AVR_PATH) avr

$(ARM_BINARY):
	@echo "** Building $@..."
	$(AT)make $(JOBS) -C $(ARM_PATH) stm32
