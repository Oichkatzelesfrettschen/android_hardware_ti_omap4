# Copyright (C) 2011 The Android Open Source Project
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

# This file lists the modules that are specific to OMAP4 but are used by
# all OMAP4 devices.

OMAP4_NEXT_FOLDER := hardware/ti/omap4

PRODUCT_PACKAGES += \
    power.omap4

PRODUCT_VENDOR_KERNEL_HEADERS := hardware/ti/omap4/kernel-headers

# Init
PRODUCT_COPY_FILES += \
    $(OMAP4_NEXT_FOLDER)/rootdir/init.omap4.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.omap4.rc

# The OMX service (media.codec) appends the vendor seccomp policy to the
# system one (main_codecservice.cpp); DOMX needs eventfd2 and pselect6, the
# SGX user-mode driver clock_nanosleep.
PRODUCT_COPY_FILES += \
    $(OMAP4_NEXT_FOLDER)/seccomp/mediacodec-seccomp.policy:$(TARGET_COPY_OUT_VENDOR)/etc/seccomp_policy/mediacodec.policy

# We don't support the new camera API
PRODUCT_PROPERTY_OVERRIDES += \
    camera2.portability.force_api=1

# Ensure release of EGL buffer in the ColorFade class
PRODUCT_PROPERTY_OVERRIDES += \
    ro.egl.destroy_after_detach=true

$(call inherit-product, hardware/ti/omap4/common.mk)
$(call inherit-product-if-exists, vendor/ti/omap4/omap4-vendor.mk)
$(call inherit-product-if-exists, vendor/widevine/arm-generic/widevine-vendor.mk)
