################################################################################
#
# rockchip-mpp
#
# Rockchip Media Process Platform - Hardware video codec library
#
################################################################################

# Use tsukumijima mirror (rockchip-linux/mpp blocked in some regions)
ROCKCHIP_MPP_VERSION = v1.5.0-1-20260121-750e76e
ROCKCHIP_MPP_SITE = $(call github,tsukumijima,mpp-rockchip,$(ROCKCHIP_MPP_VERSION))
ROCKCHIP_MPP_LICENSE = Apache-2.0
ROCKCHIP_MPP_LICENSE_FILES = LICENSE.md
ROCKCHIP_MPP_INSTALL_STAGING = YES

ROCKCHIP_MPP_CONF_OPTS = \
	-DRKPLATFORM=ON \
	-DHAVE_DRM=ON

ifeq ($(BR2_PACKAGE_ROCKCHIP_MPP_VPROC),y)
ROCKCHIP_MPP_CONF_OPTS += -DENABLE_VPROC=ON
else
ROCKCHIP_MPP_CONF_OPTS += -DENABLE_VPROC=OFF
endif

ifeq ($(BR2_PACKAGE_ROCKCHIP_MPP_TESTS),y)
ROCKCHIP_MPP_CONF_OPTS += -DBUILD_TEST=ON
else
ROCKCHIP_MPP_CONF_OPTS += -DBUILD_TEST=OFF
endif

# MPP needs access to DRM headers
ROCKCHIP_MPP_DEPENDENCIES = libdrm

$(eval $(cmake-package))
