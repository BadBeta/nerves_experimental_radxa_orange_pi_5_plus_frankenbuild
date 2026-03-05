# Nerves System for Orange Pi 5 Plus (RK3588)

Custom [Nerves](https://nerves-project.org/) system for the [Orange Pi 5 Plus](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-5-plus.html) single-board computer, with hardware-accelerated GPU, NPU, VPU, camera ISP, and display support.

Ported from the Rock 5T Nerves system — both boards share the RK3588 SoC, so GPU/NPU/VPU/ISP drivers are identical.

## Hardware Support

| Subsystem | Implementation | Details |
|-----------|---------------|---------|
| **CPU** | Cortex-A76 + A55 (big.LITTLE) | RK3588 SoC |
| **GPU** | Mali G610 (proprietary blob) | Wayland EGL/GBM via libmali |
| **NPU** | RKNPU (in-tree BSP driver) | 6 TOPS via librknnrt.so + .rknn models |
| **VPU** | Rockchip MPP | Decode: H.264, H.265, VP9, AV1, JPEG; Encode: H.264, H.265, VP8, JPEG |
| **ISP** | rkisp + rkisp1 + ispp + vpss | MIPI CSI-2 camera input |
| **CSI** | rkcif + MIPI D-PHY/C-PHY | One 4-lane MIPI CSI-2 camera input |
| **DSI** | Rockchip DRM + MIPI DC-PHY | Two 4-lane MIPI DSI display outputs |
| **Ethernet** | Dual 2.5GbE (RTL8125BG) | Both ports supported |
| **WiFi/BT** | AP6275P (BCM43752) or RTL8852BE | M.2 E-Key slot, both drivers included |
| **Audio** | ALSA (HDMI + ES8388) | Dual HDMI output, headphone jack |
| **Display** | DRM/KMS | Dual HDMI 2.1 output via Rockchip DRM |
| **HDMI Input** | V4L2 (rk_hdmirx) | Up to 4K60 capture |
| **NVMe** | M.2 M-Key PCIe 3.0 x4 | NVMe SSD support |
| **SPI NOR** | QSPI (16/32 MB) | On-board SPI NOR flash |
| **GPIO** | sysfs / libgpiod | 26-pin header with SPI, I2C, UART, PWM |

## Hardware Differences from Rock 5T

| Feature | Rock 5T | Orange Pi 5 Plus |
|---------|---------|------------------|
| HDMI out | 1x HDMI 2.1 | **2x HDMI 2.1** |
| MIPI CSI | 2x 4-lane | 1x 4-lane |
| MIPI DSI | 1x 4-lane | **2x 4-lane** |
| NVMe | No | **M.2 M-Key PCIe 3.0 x4** |
| SPI NOR | No | **16/32 MB QSPI** |
| eMMC | 64 GB (soldered) | Module socket (16-256 GB) |
| WiFi/BT | RTL8852BE (onboard) | M.2 E-Key slot (optional) |
| Audio codec | ES8316 | ES8388 |

## Kernel & Bootloader

- **Kernel**: Rockchip BSP 6.1 (`linux-6.1-stan-rkr4.1`) + Orange Pi 5 Plus DTS patch
- **U-Boot**: Mainline v2026.04-rc3 (`orangepi-5-plus-rk3588` defconfig)
- **ATF**: ARM Trusted Firmware v2.12

All sources are downloaded at build time — no local tarballs or vendor blobs to manage.

## WiFi Support

The Orange Pi 5 Plus has an M.2 E-Key slot for optional WiFi/BT modules. Both common modules are supported:

| Module | Chip | Driver | Firmware Package |
|--------|------|--------|-----------------|
| AP6275P | BCM43752 | brcmfmac (in-tree) | `brcmfmac-ap6275p` (custom) |
| RTL8852BE | RTL8852BE | rtw89 (out-of-tree) | `linux-firmware` |

## 26-Pin GPIO Header

The Orange Pi 5 Plus has a 26-pin GPIO header (not 40-pin like the Rock 5T). Pin assignments differ — consult the [Orange Pi 5 Plus schematic](http://www.orangepi.org/html/hardWare/computerAndMicrocontrollers/details/Orange-Pi-5-plus.html) for your board revision.

### Serial Console

UART2 is the default serial console at 1,500,000 baud (same as Rock 5T):

```bash
picocom -b 1500000 /dev/ttyUSB0
```

## Camera (MIPI CSI)

The Orange Pi 5 Plus has one 4-lane MIPI CSI-2 camera connector (vs two on the Rock 5T). See [`dts-overlays/`](dts-overlays/) for example camera overlays.

## Display (MIPI DSI)

Two 4-lane MIPI DSI connectors for LCD panels (DSI0 and DSI1). See [`dts-overlays/orangepi5plus-display-dsi.dts`](dts-overlays/orangepi5plus-display-dsi.dts) for an example.

## Custom Buildroot Packages

| Package | Description |
|---------|-------------|
| `rockchip-libmali-g610` | Proprietary Mali G610 userspace + EGL/GBM hook library |
| `rockchip-mpp` | Rockchip Media Process Platform (VPU) |
| `rockchip-rknpu2` | RKNN runtime library (librknnrt.so) |
| `gstreamer-rockchip` | GStreamer plugins for MPP hardware codecs |
| `rtw89-oot` | Out-of-tree RTL8852BE WiFi driver |
| `brcmfmac-ap6275p` | Broadcom AP6275P (BCM43752) WiFi/BT firmware |

## Building

### Prerequisites

- Elixir 1.17+
- Nerves Bootstrap: `mix archive.install hex nerves_bootstrap`
- Standard Nerves host dependencies ([installation guide](https://hexdocs.pm/nerves/installation.html))

### Build the System + App

```bash
cd your_app
export MIX_TARGET=orangepi5plus
mix deps.get
mix firmware
```

### DTS Patch (Required Before First Build)

The kernel DTS patch at `patches/linux/0001-arm64-dts-rockchip-add-orange-pi-5-plus-device-tree.patch` is a **placeholder**. You must populate it with the actual Orange Pi 5 Plus device tree files before building. See the instructions in the patch file header.

### Flash to MicroSD Card

Use the standard Nerves workflow to burn firmware to a MicroSD card:

```bash
mix deps.get
mix firmware
mix burn
```

Or use `fwup` directly:

```bash
fwup -a -d /dev/sdX -i _build/orangepi5plus_dev/nerves/images/your_app.fw -t complete
```

Replace `/dev/sdX` with your SD card reader device (check with `lsblk`).

## Partition Layout (GPT)

> **Note:** The partition layout in `fwup.conf` defaults to a 32GB MicroSD card. The `APP_PART_COUNT` must be adjusted for your card size (16/32/64/128 GB). See the comments in `fwup.conf`.

| Region | Offset | Size | Description |
|--------|--------|------|-------------|
| idbloader | 32 KB | ~512 KB | DDR init + SPL |
| u-boot.itb | 8 MB | ~4 MB | U-Boot + ATF + DTB |
| U-Boot env | 16 MB | 128 KB | Nerves A/B slot state |
| Boot A/B | 32 MB | 64 MB each | Kernel, DTB, boot.scr (FAT) |
| Rootfs A/B | 160 MB | 1 GB each | SquashFS (read-only) |
| App data | ~2.2 GB | ~55.5 GB | EXT4 (persistent /data) |

## Booting from eMMC

The default configuration targets MicroSD (`/dev/mmcblk1`). To boot from eMMC instead:

| File | Change |
|------|--------|
| `fwup.conf` | Change `NERVES_FW_DEVPATH` to `/dev/mmcblk0` |
| `fwup.conf` | Adjust `APP_PART_COUNT` for the eMMC module's capacity |
| `rootfs_overlay/etc/erlinit.config` | Change boot mount from `/dev/mmcblk1p1` to `/dev/mmcblk0p1` |
| `post-createfs.sh` | Change `mmcblk1p2`/`mmcblk1p3` root device references to `mmcblk0p*`, set `devnum 0` |

On RK3588, eMMC is `mmcblk0` and MicroSD is `mmcblk1`. Flash to eMMC using `rkdeveloptool` in maskrom mode (see `flash_emmc.sh` in git history).

## License

This system configuration is provided as-is. Individual components have
their own licenses (Linux kernel: GPL-2.0, Mali blob: proprietary ARM
license, Broadcom firmware: proprietary, etc.).
