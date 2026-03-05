################################################################################
#
# rockchip-rknpu2
#
# RKNN Runtime for RK3588 NPU (6 TOPS)
#
################################################################################

# Use a recent release from airockchip/rknn-toolkit2
ROCKCHIP_RKNPU2_VERSION = v2.3.0
ROCKCHIP_RKNPU2_SITE = $(call github,airockchip,rknn-toolkit2,$(ROCKCHIP_RKNPU2_VERSION))
ROCKCHIP_RKNPU2_LICENSE = Proprietary
ROCKCHIP_RKNPU2_INSTALL_STAGING = YES

# The runtime is pre-built, we just extract and copy
ROCKCHIP_RKNPU2_RUNTIME_DIR = rknpu2/runtime/Linux/librknn_api/aarch64

define ROCKCHIP_RKNPU2_INSTALL_STAGING_CMDS
	# Install runtime library to staging
	$(INSTALL) -D -m 0755 $(@D)/$(ROCKCHIP_RKNPU2_RUNTIME_DIR)/librknnrt.so \
		$(STAGING_DIR)/usr/lib/librknnrt.so

	# Install headers
	$(INSTALL) -D -m 0644 $(@D)/rknpu2/runtime/Linux/librknn_api/include/rknn_api.h \
		$(STAGING_DIR)/usr/include/rknn_api.h
	$(INSTALL) -D -m 0644 $(@D)/rknpu2/runtime/Linux/librknn_api/include/rknn_matmul_api.h \
		$(STAGING_DIR)/usr/include/rknn_matmul_api.h
endef

define ROCKCHIP_RKNPU2_INSTALL_TARGET_CMDS
	# Install runtime library
	$(INSTALL) -D -m 0755 $(@D)/$(ROCKCHIP_RKNPU2_RUNTIME_DIR)/librknnrt.so \
		$(TARGET_DIR)/usr/lib/librknnrt.so

	# Install NPU firmware if present
	if [ -d "$(@D)/rknpu2/runtime/Linux/firmware" ]; then \
		mkdir -p $(TARGET_DIR)/lib/firmware; \
		cp -r $(@D)/rknpu2/runtime/Linux/firmware/* $(TARGET_DIR)/lib/firmware/ || true; \
	fi
endef

ifeq ($(BR2_PACKAGE_ROCKCHIP_RKNPU2_DEMO),y)
define ROCKCHIP_RKNPU2_INSTALL_DEMO
	# Install benchmark tool if available
	if [ -f "$(@D)/rknpu2/examples/rknn_benchmark/install/rknn_benchmark_Linux/rknn_benchmark" ]; then \
		$(INSTALL) -D -m 0755 \
			$(@D)/rknpu2/examples/rknn_benchmark/install/rknn_benchmark_Linux/rknn_benchmark \
			$(TARGET_DIR)/usr/bin/rknn_benchmark; \
	fi
endef
ROCKCHIP_RKNPU2_POST_INSTALL_TARGET_HOOKS += ROCKCHIP_RKNPU2_INSTALL_DEMO
endif

$(eval $(generic-package))
