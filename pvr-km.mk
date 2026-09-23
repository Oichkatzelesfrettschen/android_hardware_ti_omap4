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

ifneq ($(TARGET_KERNEL_SOURCE),)

PVR_KM_SRC := $(OMAP4_NEXT_FOLDER)/pvr-source
PVR_KM_KERNEL_OUT := $(TARGET_OUT_INTERMEDIATES)/KERNEL_OBJ
PVR_KM_OUT := $(TARGET_OUT_INTERMEDIATES)/PVR_KM_OBJ
PVR_KM_TARGET_DEVICE := blaze$(if $(filter 4470,$(TARGET_BOARD_OMAP_CPU)),.4470)
PVR_KM_PVRSRVKM := $(PVR_KM_OUT)/target/pvrsrvkm.ko
PVR_KM_OMAPLFB := $(PVR_KM_OUT)/target/omaplfb.ko

$(PVR_KM_PVRSRVKM): $(PVR_KM_KERNEL_OUT)/arch/arm/boot/$(BOARD_KERNEL_IMAGE_NAME) \
		$(sort $(shell find $(PVR_KM_SRC) -type f))
	@echo "Building SGX-KM against $(PVR_KM_KERNEL_OUT)"
	$(hide) rm -rf $(PVR_KM_OUT)
	$(hide) $(PATH_OVERRIDE) $(KERNEL_MAKE_CMD) $(KERNEL_MAKE_FLAGS) \
		-C $(PVR_KM_SRC)/eurasiacon/build/linux2/omap_android \
		ARCH=$(KERNEL_ARCH) $(KERNEL_CROSS_COMPILE) \
		KERNELDIR=$(KERNEL_BUILD_OUT_PREFIX)$(PVR_KM_KERNEL_OUT) \
		OUT=$(KERNEL_BUILD_OUT_PREFIX)$(PVR_KM_OUT) \
		TARGET_DEVICE=$(PVR_KM_TARGET_DEVICE) \
		HOST_CC=$(CLANG_PREBUILTS)/bin/clang HOST_CXX=$(CLANG_PREBUILTS)/bin/clang++ \
		BUILD=release PLATFORM_RELEASE=$(PLATFORM_VERSION)
	$(hide) $(KERNEL_TOOLCHAIN_PATH)strip --strip-unneeded $(PVR_KM_PVRSRVKM) $(PVR_KM_OMAPLFB)

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
