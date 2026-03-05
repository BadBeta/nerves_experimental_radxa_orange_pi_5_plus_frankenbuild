################################################################################
#
# rockchip-libmali-g610
#
# Mali-G610 GPU userspace library for RK3588 (g24p0 + hook)
#
# Source: JeffyCN/mirrors (libmali branch) — current g24p0 blob with
# libmali-hook.so that wraps EGL 1.5 / newer GBM functions.
#
################################################################################

ROCKCHIP_LIBMALI_G610_VERSION = da680ba8d7f9bb623cdb8bfa6b087bcec24a79ff
ROCKCHIP_LIBMALI_G610_SITE = $(call github,JeffyCN,mirrors,$(ROCKCHIP_LIBMALI_G610_VERSION))
ROCKCHIP_LIBMALI_G610_LICENSE = Proprietary
ROCKCHIP_LIBMALI_G610_LICENSE_FILES = END_USER_LICENCE_AGREEMENT.txt
ROCKCHIP_LIBMALI_G610_INSTALL_STAGING = YES
ROCKCHIP_LIBMALI_G610_DEPENDENCIES = libdrm wayland wayland-protocols host-wayland

# Determine library variant based on config
ifeq ($(BR2_PACKAGE_ROCKCHIP_LIBMALI_G610_X11_WAYLAND),y)
ROCKCHIP_LIBMALI_G610_LIB = libmali-valhall-g610-g24p0-x11-wayland-gbm.so
ROCKCHIP_LIBMALI_G610_HOOK_CFLAGS = -DHAS_GBM -DHAS_EGL -DHAS_WAYLAND -DHAS_X11
else ifeq ($(BR2_PACKAGE_ROCKCHIP_LIBMALI_G610_WAYLAND),y)
ROCKCHIP_LIBMALI_G610_LIB = libmali-valhall-g610-g24p0-wayland-gbm.so
ROCKCHIP_LIBMALI_G610_HOOK_CFLAGS = -DHAS_GBM -DHAS_EGL -DHAS_WAYLAND -DEGL_NO_X11
else
ROCKCHIP_LIBMALI_G610_LIB = libmali-valhall-g610-g24p0-gbm.so
ROCKCHIP_LIBMALI_G610_HOOK_CFLAGS = -DHAS_GBM -DHAS_EGL -DEGL_NO_X11
endif

# LIBMALI_SO: used by hook.c to dlopen the real Mali blob at runtime
# Since libgbm.so now symlinks to the hook (not the blob), the hook MUST
# compile its own GBM wrapper functions that delegate to the blob via dlsym.
# Do NOT set HAS_gbm_* flags — those suppress wrapper compilation and are
# only appropriate when libgbm.so points directly to the blob.
ROCKCHIP_LIBMALI_G610_HOOK_CFLAGS += \
	'-DLIBMALI_SO="libmali.so.1"'

# Symlinks pointing to the Mali blob (raw implementation)
ROCKCHIP_LIBMALI_G610_BLOB_SYMLINKS = \
	libmali.so.1 \
	libMali.so \
	libGLESv1_CM.so.1 \
	libGLESv1_CM.so \
	libGLESv2.so.2 \
	libGLESv2.so \
	libOpenCL.so.1 \
	libOpenCL.so

# Symlinks pointing to the hook (EGL/GBM interception layer).
# The hook IS libEGL/libgbm and internally dlopen's the blob via libmali.so.1.
# This ensures ALL EGL calls (including dlsym-resolved ones) go through us.
ROCKCHIP_LIBMALI_G610_HOOK_SYMLINKS = \
	libEGL.so.1 \
	libEGL.so \
	libgbm.so.1 \
	libgbm.so

# When using Wayland variant, Mali blob provides wayland-egl symbols internally.
# The wayland package installs a standalone libwayland-egl.so that conflicts
# with Mali's internal implementation, causing eglInitialize() to fail.
ifeq ($(BR2_PACKAGE_WAYLAND),y)
ROCKCHIP_LIBMALI_G610_BLOB_SYMLINKS += \
	libwayland-egl.so.1 \
	libwayland-egl.so
endif

# ============================================================================
# Build hook library from source
# ============================================================================
define ROCKCHIP_LIBMALI_G610_BUILD_CMDS
	# Generate zwp_linux_dmabuf_v1 protocol code from wayland-protocols XML
	$(HOST_DIR)/bin/wayland-scanner client-header \
		$(STAGING_DIR)/usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml \
		$(@D)/linux-dmabuf-unstable-v1-client-protocol.h
	$(HOST_DIR)/bin/wayland-scanner private-code \
		$(STAGING_DIR)/usr/share/wayland-protocols/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml \
		$(@D)/linux-dmabuf-unstable-v1-protocol.c

	# Build libmali-hook.so (GBM/EGL wrapper + zero-copy dmabuf wayland buffer)
	$(TARGET_CC) -shared -fPIC -O3 \
		$(ROCKCHIP_LIBMALI_G610_HOOK_CFLAGS) \
		-I$(@D)/include/GBM/23.1.3 \
		-I$(@D)/include \
		-I$(@D) \
		-I$(STAGING_DIR)/usr/include \
		-I$(STAGING_DIR)/usr/include/libdrm \
		$(@D)/hook/hook.c \
		$(ROCKCHIP_LIBMALI_G610_PKGDIR)/hook_wayland_buffer.c \
		$(ROCKCHIP_LIBMALI_G610_PKGDIR)/hook_gbm_passthrough.c \
		$(ROCKCHIP_LIBMALI_G610_PKGDIR)/hook_egl_passthrough.c \
		$(@D)/linux-dmabuf-unstable-v1-protocol.c \
		-L$(STAGING_DIR)/usr/lib \
		-ldrm -ldl -lpthread -lwayland-client \
		-o $(@D)/libmali-hook.so

	# Build injector object (constructor that sets mali_injected = 1)
	$(TARGET_CC) -c -fPIC -O3 \
		$(@D)/hook/injector.c \
		-o $(@D)/injector.o
endef

# ============================================================================
# Install: blob + hook + headers + pkg-config
# ============================================================================
define ROCKCHIP_LIBMALI_G610_INSTALL_COMMON
	# Install Mali blob
	$(INSTALL) -D -m 0755 $(@D)/lib/aarch64-linux-gnu/$(ROCKCHIP_LIBMALI_G610_LIB) \
		$(1)/usr/lib/$(ROCKCHIP_LIBMALI_G610_LIB)

	# Install hook library
	$(INSTALL) -D -m 0755 $(@D)/libmali-hook.so \
		$(1)/usr/lib/libmali-hook.so

	# Create symlinks: blob-direct (GLES, OpenCL, wayland-egl)
	$(foreach symlink,$(ROCKCHIP_LIBMALI_G610_BLOB_SYMLINKS), \
		ln -sf $(ROCKCHIP_LIBMALI_G610_LIB) $(1)/usr/lib/$(symlink)$(sep))

	# Create symlinks: hook-wrapped (EGL, GBM)
	# The hook IS libEGL/libgbm so dlsym(egl_handle, "eglGetProcAddress")
	# returns OUR version, enabling eglCreateImageKHR interception.
	$(foreach symlink,$(ROCKCHIP_LIBMALI_G610_HOOK_SYMLINKS), \
		ln -sf libmali-hook.so $(1)/usr/lib/$(symlink)$(sep))

	# Install headers
	mkdir -p $(1)/usr/include
	cp -dpfr $(@D)/include/EGL $(1)/usr/include/
	cp -dpfr $(@D)/include/GLES $(1)/usr/include/
	cp -dpfr $(@D)/include/GLES2 $(1)/usr/include/
	cp -dpfr $(@D)/include/GLES3 $(1)/usr/include/
	cp -dpfr $(@D)/include/KHR $(1)/usr/include/
	-cp -dpfr $(@D)/include/CL $(1)/usr/include/
	# GBM header: use 23.1.3 (newest, matches hook expectations)
	$(INSTALL) -D -m 0644 $(@D)/include/GBM/23.1.3/gbm.h $(1)/usr/include/gbm.h

	# Generate pkg-config files
	mkdir -p $(1)/usr/lib/pkgconfig
	printf 'prefix=/usr\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: egl\nDescription: Mali EGL library\nVersion: 1.5\nLibs: -L$${libdir} -lEGL\nCflags: -I$${includedir}\n' \
		> $(1)/usr/lib/pkgconfig/egl.pc
	printf 'prefix=/usr\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: glesv2\nDescription: Mali OpenGL ES 2.0 library\nVersion: 2.0\nLibs: -L$${libdir} -lGLESv2\nCflags: -I$${includedir}\n' \
		> $(1)/usr/lib/pkgconfig/glesv2.pc
	printf 'prefix=/usr\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: gbm\nDescription: Mali GBM library\nVersion: 23.1.3\nLibs: -L$${libdir} -lgbm\nCflags: -I$${includedir}\n' \
		> $(1)/usr/lib/pkgconfig/gbm.pc
	printf 'prefix=/usr\nlibdir=$${prefix}/lib\nincludedir=$${prefix}/include\n\nName: wayland-egl\nDescription: Mali wayland-egl library\nVersion: 18.1.0\nLibs: -L$${libdir} -lwayland-egl\nRequires: wayland-client\n' \
		> $(1)/usr/lib/pkgconfig/wayland-egl.pc
endef

define ROCKCHIP_LIBMALI_G610_INSTALL_STAGING_CMDS
	$(call ROCKCHIP_LIBMALI_G610_INSTALL_COMMON,$(STAGING_DIR))
endef

define ROCKCHIP_LIBMALI_G610_INSTALL_TARGET_CMDS
	$(call ROCKCHIP_LIBMALI_G610_INSTALL_COMMON,$(TARGET_DIR))

	# Install firmware
	$(INSTALL) -D -m 0644 $(@D)/firmware/g610/mali_csffw.bin \
		$(TARGET_DIR)/lib/firmware/mali_csffw.bin

	# OpenCL ICD configuration
	mkdir -p $(TARGET_DIR)/etc/OpenCL/vendors
	echo "/usr/lib/$(ROCKCHIP_LIBMALI_G610_LIB)" > \
		$(TARGET_DIR)/etc/OpenCL/vendors/mali.icd
endef

# Remove the standalone wayland-egl library installed by the wayland package.
# Mali's blob provides these symbols internally; the standalone version creates
# incompatible wl_egl_window structures that cause eglInitialize() to fail.
ifeq ($(BR2_PACKAGE_WAYLAND),y)
define ROCKCHIP_LIBMALI_G610_FIXUP_WAYLAND_EGL
	rm -f $(TARGET_DIR)/usr/lib/libwayland-egl.so.1.*
	rm -f $(STAGING_DIR)/usr/lib/libwayland-egl.so.1.*
endef
ROCKCHIP_LIBMALI_G610_POST_INSTALL_TARGET_HOOKS += ROCKCHIP_LIBMALI_G610_FIXUP_WAYLAND_EGL
endif

$(eval $(generic-package))
