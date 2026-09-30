# Copyright (C) 2016 The Unlegacy Android Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

# SGX-KM: pvrsrvkm.ko and omaplfb.ko built against the kernel of this
# build. The modules carry the kernel's vermagic and, with
# CONFIG_MODVERSIONS, the CRC of every symbol they import, so they load only
# into the kernel they were built against. The recipe reuses the toolchain,
# make and PATH that vendor/lineage/build/tasks/kernel.mk hands the kernel
# build; those variables are expanded when the recipe runs, after
# kernel.mk has been read.
# modpost only warns about an import nothing exports, and the kernel then
# refuses to load the module, so the recipe fails on any such import.

ifneq ($(TARGET_KERNEL_SOURCE),)

PVR_KM_SRC := $(OMAP4_NEXT_FOLDER)/pvr-source
PVR_KM_KERNEL_OUT := $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ
PVR_KM_OUT := $(TARGET_OUT_INTERMEDIATES)/PVR_KM_OBJ
PVR_KM_TARGET_DEVICE := blaze$(if $(filter 4470,$(TARGET_BOARD_OMAP_CPU)),.4470)
PVR_KM_PVRSRVKM := $(PVR_KM_OUT)/target/pvrsrvkm.ko
PVR_KM_OMAPLFB := $(PVR_KM_OUT)/target/omaplfb.ko

# The product supplies the same LLVM directory and IAS mode as kernel.mk.
# Keep the GCC toolchain selected when PVR_KM_LLVM is empty.
ifneq ($(strip $(PVR_KM_LLVM)),)
export PVR_KM_LLVM
PVR_KM_LLVM_INVALID := $(shell printf '%s' "$$PVR_KM_LLVM" | LC_ALL=C tr -d 'A-Za-z0-9_./+-' | od -An -tx1)
ifneq ($(strip $(PVR_KM_LLVM_INVALID)),)
$(error PVR_KM_LLVM contains unsafe path characters)
endif
ifneq ($(words $(PVR_KM_LLVM)),1)
$(error PVR_KM_LLVM must be one absolute directory without spaces)
endif
ifeq ($(filter /%/,$(PVR_KM_LLVM)),)
$(error PVR_KM_LLVM must be an absolute directory ending in /)
endif
ifeq ($(filter $(PVR_KM_LLVM_IAS),0 1),)
$(error PVR_KM_LLVM_IAS must be 0 or 1)
endif
PVR_KM_CC := $(PVR_KM_LLVM)clang
PVR_KM_LD := $(PVR_KM_LLVM)ld.lld
PVR_KM_AR := $(PVR_KM_LLVM)llvm-ar
PVR_KM_RANLIB := $(PVR_KM_LLVM)llvm-ranlib
PVR_KM_NM := $(PVR_KM_LLVM)llvm-nm
PVR_KM_OBJCOPY := $(PVR_KM_LLVM)llvm-objcopy
PVR_KM_OBJDUMP := $(PVR_KM_LLVM)llvm-objdump
PVR_KM_STRIP := $(PVR_KM_LLVM)llvm-strip
PVR_KM_KERNEL_TOOLS := LLVM=$(PVR_KM_LLVM) LLVM_IAS=$(PVR_KM_LLVM_IAS) \
	CC=$(PVR_KM_CC) LD=$(PVR_KM_LD) AR=$(PVR_KM_AR) \
	RANLIB=$(PVR_KM_RANLIB) NM=$(PVR_KM_NM) \
	OBJCOPY=$(PVR_KM_OBJCOPY) OBJDUMP=$(PVR_KM_OBJDUMP) \
	STRIP=$(PVR_KM_STRIP) \
	KERNEL_CC=$(PVR_KM_CC) KERNEL_LD=$(PVR_KM_LD) \
	KERNEL_AR=$(PVR_KM_AR) KERNEL_NM=$(PVR_KM_NM) \
	KERNEL_OBJCOPY=$(PVR_KM_OBJCOPY) KERNEL_OBJDUMP=$(PVR_KM_OBJDUMP) \
	KERNEL_STRIP=$(PVR_KM_STRIP)
else
PVR_KM_NM := $(KERNEL_TOOLCHAIN_PATH)nm
PVR_KM_STRIP := $(KERNEL_TOOLCHAIN_PATH)strip
endif

$(PVR_KM_PVRSRVKM): $(PVR_KM_KERNEL_OUT)/arch/arm/boot/$(BOARD_KERNEL_IMAGE_NAME) \
		$(sort $(shell find $(PVR_KM_SRC) -type f))
	@echo "Building SGX-KM against $(PVR_KM_KERNEL_OUT)"
	$(hide) rm -rf $(PVR_KM_OUT)
	$(hide) $(PATH_OVERRIDE) $(KERNEL_MAKE_CMD) $(KERNEL_MAKE_FLAGS) \
		-C $(PVR_KM_SRC)/eurasiacon/build/linux2/omap_android \
		ARCH=$(KERNEL_ARCH) $(KERNEL_CROSS_COMPILE) \
		$(PVR_KM_KERNEL_TOOLS) \
		KERNELDIR=$(KERNEL_BUILD_OUT_PREFIX)$(PVR_KM_KERNEL_OUT) \
		OUT=$(KERNEL_BUILD_OUT_PREFIX)$(PVR_KM_OUT) \
		TARGET_DEVICE=$(PVR_KM_TARGET_DEVICE) \
		HOST_CC=$(CLANG_PREBUILTS)/bin/clang HOST_CXX=$(CLANG_PREBUILTS)/bin/clang++ \
		BUILD=release PLATFORM_RELEASE=$(PLATFORM_VERSION)
	$(hide) $(PVR_KM_STRIP) --strip-unneeded $(PVR_KM_PVRSRVKM) $(PVR_KM_OMAPLFB)
	$(hide) cat $(PVR_KM_KERNEL_OUT)/Module.symvers $(PVR_KM_OUT)/target/kbuild/Module.symvers \
		| awk '{ print $$2 }' | sort -u > $(PVR_KM_OUT)/exported.txt
	$(hide) for ko in $(PVR_KM_PVRSRVKM) $(PVR_KM_OMAPLFB); do \
		nm_output=$$($(PVR_KM_NM) -u "$$ko") || exit 1; \
		missing=$$(printf '%s\n' "$$nm_output" | awk '{ print $$2 }' | sort -u \
			| comm -23 - $(PVR_KM_OUT)/exported.txt); \
		if [ -n "$$missing" ]; then \
			echo "$$ko imports symbols no kernel or module exports:" $$missing >&2; \
			exit 1; \
		fi; \
	done

$(PVR_KM_OMAPLFB): $(PVR_KM_PVRSRVKM)
	$(hide) test -s $@

include $(CLEAR_VARS)
LOCAL_MODULE := pvrsrvkm.ko
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/lib/modules
LOCAL_PREBUILT_MODULE_FILE := $(PVR_KM_PVRSRVKM)
include $(BUILD_PREBUILT)

include $(CLEAR_VARS)
LOCAL_MODULE := omaplfb.ko
LOCAL_MODULE_CLASS := ETC
LOCAL_MODULE_PATH := $(TARGET_OUT_VENDOR)/lib/modules
LOCAL_PREBUILT_MODULE_FILE := $(PVR_KM_OMAPLFB)
LOCAL_REQUIRED_MODULES := pvrsrvkm.ko
include $(BUILD_PREBUILT)

endif
