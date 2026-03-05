################################################################################
#
# rtw89-oot (out-of-tree rtw89 driver for RTL8852BE WiFi)
#
# This package provides RTL8852BE support that is missing from the BSP kernel.
# Uses a5a5aa555oo/rtw89 fork which supports kernel 5.15-6.5 (BSP 6.1).
# NOTE: lwfinger/rtw89 requires kernel 6.10+ and fails auth on BSP 6.1.
#
# For Collabora 6.18, use in-tree CONFIG_RTW89 instead of this OOT package.
#
################################################################################

RTW89_OOT_VERSION = 2310939e2002117cd2b223c6d4f997e537142b78
RTW89_OOT_SITE = $(call github,a5a5aa555oo,rtw89,$(RTW89_OOT_VERSION))
RTW89_OOT_LICENSE = GPL-2.0
RTW89_OOT_LICENSE_FILES = LICENSE

# Use kernel-module infrastructure for proper cross-compilation
RTW89_OOT_DEPENDENCIES = linux

# The driver's Makefile uses KDIR to find kernel source
define RTW89_OOT_BUILD_CMDS
	$(MAKE) -C $(@D) $(LINUX_MAKE_FLAGS) \
		KDIR=$(LINUX_DIR) \
		KVER=$(LINUX_VERSION_PROBED) \
		ARCH=$(KERNEL_ARCH) \
		CROSS_COMPILE=$(TARGET_CROSS)
endef

define RTW89_OOT_INSTALL_TARGET_CMDS
	# Install kernel modules
	$(MAKE) -C $(LINUX_DIR) $(LINUX_MAKE_FLAGS) \
		M=$(@D) \
		INSTALL_MOD_PATH=$(TARGET_DIR) \
		INSTALL_MOD_STRIP=1 \
		modules_install
	# Run depmod to update module dependencies
	$(HOST_DIR)/sbin/depmod -a -b $(TARGET_DIR) $(LINUX_VERSION_PROBED) || true
	# Install modprobe configuration for proper module loading order
	$(INSTALL) -D -m 0644 $(@D)/70-rtw89.conf \
		$(TARGET_DIR)/etc/modprobe.d/70-rtw89.conf 2>/dev/null || true
endef

$(eval $(generic-package))
