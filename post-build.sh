#!/bin/bash

# post-build.sh - Post-build script for Orange Pi 5 Plus Nerves system
#
# This script runs after Buildroot builds the target filesystem
# but before the rootfs image is created.

set -e

TARGETDIR=$1
NERVES_DEFCONFIG_DIR="${0%/*}"

# Print build info
echo "Running Orange Pi 5 Plus post-build script..."
echo "Target directory: ${TARGETDIR}"
echo "Nerves defconfig directory: ${NERVES_DEFCONFIG_DIR}"

# Create required directories
mkdir -p "${TARGETDIR}/srv/erlang"
mkdir -p "${TARGETDIR}/root"
mkdir -p "${TARGETDIR}/data"
mkdir -p "${TARGETDIR}/mnt"

# Set permissions on key directories
chmod 700 "${TARGETDIR}/root"
chmod 755 "${TARGETDIR}/srv"
chmod 755 "${TARGETDIR}/data"

# Copy any additional firmware files if they exist
if [ -d "${NERVES_DEFCONFIG_DIR}/firmware" ]; then
    mkdir -p "${TARGETDIR}/lib/firmware"
    cp -r "${NERVES_DEFCONFIG_DIR}/firmware/"* "${TARGETDIR}/lib/firmware/" 2>/dev/null || true
fi

# Ensure the erlinit.config is in place
if [ -f "${NERVES_DEFCONFIG_DIR}/rootfs_overlay/etc/erlinit.config" ]; then
    mkdir -p "${TARGETDIR}/etc"
    cp "${NERVES_DEFCONFIG_DIR}/rootfs_overlay/etc/erlinit.config" "${TARGETDIR}/etc/"
fi

# Add self-signed TLS cert to CA trust store so WPE WebKit trusts wss://localhost:443
# (Required for YOLO/Whisper WebSocket from HTTPS YouTube pages)
KIOSK_CERT="${NERVES_DEFCONFIG_DIR}/rootfs_overlay/etc/tls/cert.pem"
CA_BUNDLE="${TARGETDIR}/etc/ssl/certs/ca-certificates.crt"
if [ -f "${KIOSK_CERT}" ] && [ -f "${CA_BUNDLE}" ]; then
    echo "" >> "${CA_BUNDLE}"
    echo "# OPi5Plus Kiosk localhost self-signed cert" >> "${CA_BUNDLE}"
    cat "${KIOSK_CERT}" >> "${CA_BUNDLE}"
    echo "Added self-signed cert to CA trust store"
fi

# Clean up unnecessary files to reduce rootfs size
rm -rf "${TARGETDIR}/usr/share/man"
rm -rf "${TARGETDIR}/usr/share/doc"
rm -rf "${TARGETDIR}/usr/share/info"

# Remove static libraries (keep only shared)
find "${TARGETDIR}/usr/lib" -name "*.a" -delete 2>/dev/null || true

echo "Orange Pi 5 Plus post-build script completed."
