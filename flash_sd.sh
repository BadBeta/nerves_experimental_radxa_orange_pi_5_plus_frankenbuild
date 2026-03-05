#!/bin/bash
# Flash Nerves firmware to MicroSD card for Orange Pi 5 Plus
#
# Standard Nerves workflow:
#   mix deps.get && mix firmware && mix burn
#
# This script provides a standalone alternative using fwup directly.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FW_FILE="${1:-$SCRIPT_DIR/test_app/_build/orangepi5plus_dev/nerves/images/test_app.fw}"
DEVICE="${2:-}"

echo "=== Orange Pi 5 Plus MicroSD Flash ==="
echo ""

[ -f "$FW_FILE" ] || { echo "ERROR: Firmware not found: $FW_FILE"; echo "Usage: $0 [firmware.fw] [/dev/sdX]"; exit 1; }

echo "Firmware: $FW_FILE"

if [ -z "$DEVICE" ]; then
    echo ""
    echo "Available removable block devices:"
    lsblk -d -o NAME,SIZE,TRAN,MODEL | grep -E "usb|sd" || true
    echo ""
    echo "No device specified. Usage: $0 [firmware.fw] /dev/sdX"
    echo "Or use: mix burn"
    exit 1
fi

echo "Target:   $DEVICE"
echo ""
read -p "This will ERASE $DEVICE. Continue? [y/N] " confirm
[ "$confirm" = "y" ] || [ "$confirm" = "Y" ] || { echo "Aborted."; exit 1; }

echo ""
echo "Flashing firmware to $DEVICE..."
sudo fwup -a -d "$DEVICE" -i "$FW_FILE" -t complete

echo ""
echo "=== Flash complete! ==="
echo "Insert the MicroSD card into the Orange Pi 5 Plus and power on."
echo "Watch serial console: picocom -b 1500000 /dev/ttyUSB0"
