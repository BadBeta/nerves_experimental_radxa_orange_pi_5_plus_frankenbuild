#!/bin/bash

# post-createfs.sh - Post image creation script for Orange Pi 5 Plus Nerves system
#
# This script runs after the rootfs image is created.
# It assembles the final firmware image with bootloader components.

set -e

BINARIES_DIR=$1
NERVES_DEFCONFIG_DIR="${0%/*}"

# Run the common Nerves post-image processing.
# This symlinks scripts/, nerves.mk, nerves-env.sh into $BASE_DIR
# and copies fwup.conf to $BINARIES_DIR.
FWUP_CONFIG="${NERVES_DEFCONFIG_DIR}/fwup.conf"
"$BR2_EXTERNAL_NERVES_PATH/board/nerves-common/post-createfs.sh" "$TARGET_DIR" "$FWUP_CONFIG"

# Find the build directory
BUILD_DIR=$(dirname "${BINARIES_DIR}")/build

echo "Running Orange Pi 5 Plus post-createfs script..."
echo "Binaries directory: ${BINARIES_DIR}"
echo "Build directory: ${BUILD_DIR}"

# Verify required files exist
required_files=(
    "Image"
    "rootfs.squashfs"
)

for file in "${required_files[@]}"; do
    if [ ! -f "${BINARIES_DIR}/${file}" ]; then
        echo "ERROR: Required file missing: ${BINARIES_DIR}/${file}"
        exit 1
    fi
done

# Check for device tree
DTB_FILE="${BINARIES_DIR}/rk3588-orangepi-5-plus.dtb"
if [ ! -f "${DTB_FILE}" ]; then
    # Try alternative naming
    DTB_FILE=$(find "${BINARIES_DIR}" -name "*orangepi*5*plus*.dtb" 2>/dev/null | head -1)
    if [ -z "${DTB_FILE}" ]; then
        echo "ERROR: Device tree file not found for Orange Pi 5 Plus"
        exit 1
    fi
    echo "Using device tree: ${DTB_FILE}"
fi

# ============================================
# Create Rockchip boot images
# ============================================
# RK3588 boot flow: DDR init (TPL) -> SPL -> ATF (BL31) -> U-Boot
# idbloader.img = DDR blob + SPL
# u-boot.itb = ATF + U-Boot (FIT image)

echo ""
echo "Creating Rockchip boot images..."

# ============================================
# Create idbloader.img (DDR blob + SPL)
# ============================================
# Radxa U-Boot fork doesn't generate idbloader.img automatically.
# We create it by combining the rkbin DDR blob with U-Boot SPL.

UBOOT_DIR=$(find "${BUILD_DIR}" -maxdepth 1 -type d -name "uboot-*" 2>/dev/null | head -1)
if [ -z "${UBOOT_DIR}" ]; then
    echo "ERROR: U-Boot build directory not found"
    exit 1
fi

MKIMAGE="${UBOOT_DIR}/tools/mkimage"
if [ ! -x "${MKIMAGE}" ]; then
    echo "ERROR: mkimage not found in U-Boot build"
    exit 1
fi

SPL_BIN="${UBOOT_DIR}/spl/u-boot-spl.bin"
DDR_BLOB=$(find "${BINARIES_DIR}" -name "rk3588_ddr_*.bin" 2>/dev/null | head -1)

if [ ! -f "${SPL_BIN}" ]; then
    echo "ERROR: u-boot-spl.bin not found"
    exit 1
fi

if [ -z "${DDR_BLOB}" ]; then
    echo "ERROR: DDR blob not found in ${BINARIES_DIR}"
    exit 1
fi

echo "Creating idbloader.img from:"
echo "  DDR blob: ${DDR_BLOB}"
echo "  SPL:      ${SPL_BIN}"

"${MKIMAGE}" -n rk3588 -T rksd -d "${DDR_BLOB}":"${SPL_BIN}" "${BINARIES_DIR}/idbloader.img"
echo "Created idbloader.img"

# Verify u-boot.itb
if [ ! -f "${BINARIES_DIR}/u-boot.itb" ]; then
    echo "ERROR: u-boot.itb not found — U-Boot build may have failed"
    exit 1
fi

echo "U-Boot images ready"

# ============================================
# Generate boot.scr (U-Boot boot script)
# ============================================
echo ""
echo "Creating boot script..."

BOOT_CMD="${BINARIES_DIR}/boot.cmd"
BOOT_SCR="${BINARIES_DIR}/boot.scr"

# Read kernel command line from cmdline.txt (minus root= which is set dynamically)
CMDLINE_FILE="${NERVES_DEFCONFIG_DIR}/cmdline.txt"
if [ -f "${CMDLINE_FILE}" ]; then
    # Strip root= and rootfstype= from cmdline — boot script sets these dynamically
    KERNEL_CMDLINE=$(cat "${CMDLINE_FILE}" | tr -d '\n' | sed 's/root=[^ ]* *//g; s/rootfstype=[^ ]* *//g')
    echo "Using cmdline.txt (without root=): ${KERNEL_CMDLINE}"
else
    KERNEL_CMDLINE="rootwait ro init=/sbin/init console=ttyS2,1500000n8 loglevel=7 net.ifnames=0 clk_ignore_unused regulator_ignore_unused"
    echo "WARNING: cmdline.txt not found, using default"
fi

# U-Boot env location must match fwup.conf UBOOT_ENV_OFFSET
# UBOOT_ENV_OFFSET = 32768 sectors = 0x8000 hex sectors
# UBOOT_ENV_COUNT = 256 sectors = 0x100 hex sectors = 128KB
UBOOT_ENV_SECTOR="0x8000"
UBOOT_ENV_SECTORS="0x100"
UBOOT_ENV_SIZE="0x20000"

cat > "${BOOT_CMD}" << EOF
# Boot script for Orange Pi 5 Plus Nerves (A/B partition switching)
#
# This script reads nerves_fw_active from U-Boot env to determine
# which rootfs partition to boot from:
#   active=a -> root=/dev/mmcblk0p2 (rootfs-a, GPT partition 1)
#   active=b -> root=/dev/mmcblk0p3 (rootfs-b, GPT partition 2)
#
# The boot FAT partition (kernel, DTB, boot.scr) is always GPT partition 0
# (mmc 0:1 in U-Boot). fwup rewrites the GPT to point partition 0 at
# either BOOT-A or BOOT-B offset.

echo "Orange Pi 5 Plus Nerves Boot"

# Set memory addresses for RK3588
if test -z "\${kernel_addr_r}"; then
    setenv kernel_addr_r 0x00400000
fi
if test -z "\${fdt_addr_r}"; then
    setenv fdt_addr_r 0x08300000
fi

# Boot device (eMMC = 0)
if test -z "\${devnum}"; then
    setenv devnum 0
fi

# ============================================================
# Determine active slot (A/B) from U-Boot environment
# ============================================================
# Load the Nerves U-Boot env block from eMMC.
# The env is at sector ${UBOOT_ENV_SECTOR} (${UBOOT_ENV_SIZE} bytes).
# fwup writes standard U-Boot env format (CRC32 + key=value pairs).
setenv nerves_fw_active

# Try reading env from eMMC and importing it
if mmc dev \${devnum}; then
    if mmc read 0x02000000 ${UBOOT_ENV_SECTOR} ${UBOOT_ENV_SECTORS}; then
        env import -b 0x02000000 ${UBOOT_ENV_SIZE} nerves_fw_active
    fi
fi

# Select rootfs partition based on active slot
if test "\${nerves_fw_active}" = "b"; then
    setenv nerves_root /dev/mmcblk0p3
    echo "Active slot: B (rootfs-b)"
else
    setenv nerves_root /dev/mmcblk0p2
    setenv nerves_fw_active a
    echo "Active slot: A (rootfs-a)"
fi

# Set boot arguments with dynamic root=
setenv bootargs "root=\${nerves_root} rootfstype=squashfs ${KERNEL_CMDLINE}"

# Load kernel from boot FAT partition
fatload mmc \${devnum}:1 \${kernel_addr_r} Image
fatload mmc \${devnum}:1 \${fdt_addr_r} rk3588-orangepi-5-plus.dtb

# Boot
booti \${kernel_addr_r} - \${fdt_addr_r}

echo "ERROR: booti failed!"
EOF

# Create boot.scr using mkimage
# Prefer Buildroot host mkimage over U-Boot's tools/mkimage.
# The Radxa BSP U-Boot mkimage has a bug: it writes 0xFFFFFFFF as the
# multi-image terminator instead of 0x00000000, causing U-Boot to
# misparse the script payload offset.
MKIMAGE_HOST="${HOST_DIR}/bin/mkimage"
if [ -x "${MKIMAGE_HOST}" ]; then
    "${MKIMAGE_HOST}" -A arm64 -O linux -T script -C none -d "${BOOT_CMD}" "${BOOT_SCR}"
    echo "Created boot.scr (host mkimage)"
elif command -v mkimage >/dev/null 2>&1; then
    mkimage -A arm64 -O linux -T script -C none -d "${BOOT_CMD}" "${BOOT_SCR}"
    echo "Created boot.scr (system mkimage)"
elif [ -x "${MKIMAGE}" ]; then
    "${MKIMAGE}" -A arm64 -O linux -T script -C none -d "${BOOT_CMD}" "${BOOT_SCR}"
    echo "WARNING: Created boot.scr using U-Boot mkimage (may have multi-image terminator bug)"
else
    echo "ERROR: mkimage not found, boot.scr not created"
    exit 1
fi

# ============================================
# Copy additional files
# ============================================
echo ""
echo "Copying additional files..."

# Copy cmdline.txt to binaries
if [ -f "${NERVES_DEFCONFIG_DIR}/cmdline.txt" ]; then
    cp "${NERVES_DEFCONFIG_DIR}/cmdline.txt" "${BINARIES_DIR}/"
fi

# Create symlinks for fwup.conf expected names
ln -sf "rootfs.squashfs" "${BINARIES_DIR}/rootfs.img"

# Copy fwup-revert configuration (fwup.conf is already copied by the common script)
cp "${NERVES_DEFCONFIG_DIR}/fwup-revert.conf" "${BINARIES_DIR}/"

echo ""
echo "Orange Pi 5 Plus post-createfs script completed."
echo ""
echo "Output files in ${BINARIES_DIR}:"
ls -la "${BINARIES_DIR}/"*.img "${BINARIES_DIR}/"*.itb "${BINARIES_DIR}/"*.scr "${BINARIES_DIR}/"*.dtb "${BINARIES_DIR}/"*.bin 2>/dev/null | head -20 || true
