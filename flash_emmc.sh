#!/bin/bash
# Flash Nerves firmware to Orange Pi 5 Plus eMMC via rkdeveloptool
# Device must be in MASKROM mode

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ARTIFACT_DIR=$(find "$SCRIPT_DIR/.nerves/artifacts" -maxdepth 1 -type d -name "nerves_system_orangepi5plus-portable-*" 2>/dev/null | sort -V | tail -1)
# Prefer loader from dl/ (manually placed), fall back to build artifacts
LOADER=$(find "$SCRIPT_DIR/dl" -name "rk3588_spl_loader*.bin" 2>/dev/null | sort -V | tail -1)
[ -z "$LOADER" ] && LOADER=$(find "$ARTIFACT_DIR/build" -name "rk3588_spl_loader*.bin" 2>/dev/null | head -1)
FW_FILE="$SCRIPT_DIR/test_app/_build/orangepi5plus_dev/nerves/images/test_app.fw"
IMAGE="/tmp/orangepi5plus_test.img"

echo "=== Orange Pi 5 Plus eMMC Flash (Maskrom Mode) ==="
echo ""

# Check files exist
[ -f "$LOADER" ] || { echo "ERROR: Loader not found: $LOADER"; exit 1; }
[ -f "$FW_FILE" ] || { echo "ERROR: Firmware not found: $FW_FILE"; exit 1; }

echo "Loader: $LOADER"
echo "Firmware: $FW_FILE"
echo ""

# Step 1: Create full image from .fw file
echo "Step 1: Creating disk image from firmware..."
rm -f "$IMAGE"
fwup -a -d "$IMAGE" -i "$FW_FILE" -t complete --unsafe

# Step 2: Truncate to essential data, then de-sparse
# Layout: rootfs-a ends at sector (327680 + 2097152) = 2424832
# Add 1MB padding = 2426880 sectors = 1184+1 MB
# fwup creates a sparse file; rkdeveloptool skips sparse holes,
# leaving old eMMC data intact. De-sparse so zeros are written.
echo "Step 2: Preparing image for flash..."
TRUNCATE_SECTORS=2426880
TRUNCATE_BYTES=$((TRUNCATE_SECTORS * 512))
truncate -s $TRUNCATE_BYTES "$IMAGE"
DENSE_IMAGE="/tmp/orangepi5plus_test_dense.img"
rm -f "$DENSE_IMAGE"
dd if="$IMAGE" of="$DENSE_IMAGE" bs=4M status=progress
rm -f "$IMAGE"
IMAGE="$DENSE_IMAGE"
echo "Created: $IMAGE ($(du -h "$IMAGE" | cut -f1))"
echo ""

# Step 3: Check device
echo "Step 3: Checking for Rockchip device..."
rkdeveloptool ld
echo ""

# Step 4: Download loader (initializes DRAM)
echo "Step 4: Downloading loader (initializes DRAM)..."
rkdeveloptool db "$LOADER"
sleep 2
echo ""

# Step 5: Write image to eMMC
echo "Step 5: Writing image to eMMC..."
rkdeveloptool wl 0 "$IMAGE"
echo ""

# Step 6: Reboot
echo "Step 6: Rebooting..."
rkdeveloptool rd
echo ""

echo "=== Flash complete! ==="
echo "Watch serial console: picocom -b 1500000 /dev/ttyUSB0"
