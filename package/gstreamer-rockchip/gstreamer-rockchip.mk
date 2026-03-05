################################################################################
#
# gstreamer-rockchip
#
################################################################################

# Meonardo/gst-rockchip main branch (meson-based, GStreamer 1.22.6 compatible)
GSTREAMER_ROCKCHIP_VERSION = main
GSTREAMER_ROCKCHIP_SITE = $(call github,Meonardo,gst-rockchip,$(GSTREAMER_ROCKCHIP_VERSION))
GSTREAMER_ROCKCHIP_LICENSE = LGPL-2.1+
GSTREAMER_ROCKCHIP_LICENSE_FILES = COPYING

GSTREAMER_ROCKCHIP_DEPENDENCIES = \
	rockchip-mpp \
	gstreamer1 \
	gst1-plugins-base

GSTREAMER_ROCKCHIP_CONF_OPTS = \
	-Drkximage=disabled \
	-Dkmssrc=disabled \
	-Drga=disabled \
	-Dvpxalphadec=disabled \
	-Drockchipmpp=enabled

$(eval $(meson-package))
