# Device Tree Overlays

Device tree overlays modify the base device tree at boot time to enable
additional hardware (cameras, displays, sensors) without rebuilding the
entire kernel.

## What's Here

| File | Description |
|------|-------------|
| `orangepi5plus-camera-imx219.dts` | IMX219 (RPi Camera v2) on CAM0 |
| `orangepi5plus-display-dsi.dts` | MIPI DSI panel on DSI0 |

These are **example templates** — you will need to adjust GPIO pins,
I2C addresses, and lane counts for your specific hardware.

## Compiling an Overlay

```bash
dtc -@ -I dts -O dtb -o orangepi5plus-camera-imx219.dtbo orangepi5plus-camera-imx219.dts
```

The `-@` flag is required to produce a proper overlay with symbol
references.

## Adding to the Build

1. Copy the compiled `.dtbo` to `rootfs_overlay/lib/firmware/` (or
   another path accessible at boot).

2. In U-Boot, load and apply the overlay before booting:

   ```
   fatload mmc 0:1 ${fdt_addr} rk3588-orangepi-5-plus.dtb
   fdt addr ${fdt_addr}
   fatload mmc 0:1 0x10000000 orangepi5plus-camera-imx219.dtbo
   fdt apply 0x10000000
   ```

   Alternatively, integrate the overlay into your `boot.scr` or
   `extlinux.conf`.

3. For Nerves, the simplest approach is to merge the overlay nodes
   directly into the kernel DTS patch
   (`patches/linux/0001-arm64-dts-rockchip-add-orange-pi-5-plus-*.patch`) and
   rebuild.
