################################################################################
#
# rknpu — Rockchip RKNPU kernel driver for mainline Linux
#
# Adapted from BSP driver. Uses dma_alloc_coherent() instead of rk_dma_heap.
# Creates /dev/rknpu misc device.
#
################################################################################

RKNPU_VERSION = 1.0.0
RKNPU_SITE = $(NERVES_DEFCONFIG_DIR)/package/rknpu/src
RKNPU_SITE_METHOD = local
RKNPU_LICENSE = GPL-2.0
RKNPU_DEPENDENCIES = linux

define RKNPU_BUILD_CMDS
	$(MAKE) -C $(LINUX_DIR) $(LINUX_MAKE_FLAGS) \
		M=$(@D) \
		ARCH=$(KERNEL_ARCH) \
		CROSS_COMPILE=$(TARGET_CROSS) \
		modules
endef

define RKNPU_INSTALL_TARGET_CMDS
	$(MAKE) -C $(LINUX_DIR) $(LINUX_MAKE_FLAGS) \
		M=$(@D) \
		INSTALL_MOD_PATH=$(TARGET_DIR) \
		INSTALL_MOD_STRIP=1 \
		modules_install
	$(HOST_DIR)/sbin/depmod -a -b $(TARGET_DIR) $(LINUX_VERSION_PROBED) || true
endef

$(eval $(generic-package))
