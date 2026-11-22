# Mainline Linux for Infinix Note 12 Pro (X676B, MediaTek MT6789 / Helio G99)

Fork of [ellyq/mtk_g99-mainline](https://gitlab.postmarketos.org/ellyq/mtk_g99-mainline)
with support for the **Infinix Note 12 Pro (X676B)** added.

Device: MT6789 (Helio G99), 8 GiB RAM, 256 GiB UFS, 1080x2400 AMOLED.
Bootloader: U-Boot packed into the stock LK partition (see the separate
`u-boot-mtk-g99` / `x676b_tools_linux` work). Root filesystem: Arch Linux ARM
on a UFS partition, systemd.

## Status (tested on hardware)

### Works

| Subsystem | Notes |
|---|---|
| Boot | Arch Linux ARM on UFS (`sdc63`), systemd; Alpine on `sdc62` as fallback |
| RAM | full 8 GiB visible and usable (two banks) |
| USB | Type-C composite gadget: ACM console + RNDIS network (SSH) |
| Storage | UFS read/write (root filesystem) |
| SD card | MSDC1: 128 GiB SDXC, 50 MHz / 4-bit / 3.3 V, card-detect hotplug (insert and remove) |
| PMIC | MT6366 via pwrap: regulators, RTC, power/reset keys, ADC |
| CPU frequency | 2 clusters (6x A55 up to 2.0 GHz, 2x A76 up to 2.2 GHz), schedutil |
| Thermal | MT6789 LVTS, 13 zones, 110 C critical trips |
| Display | custom `x676b_drm` DRM/KMS module: fbcon on the ILI7835 AMOLED, OVL0 layer 1 scans the reserved DMA buffer at `0xfeabe000`, DCS 0x2C trigger every 16 ms |
| Audio | MT6789 AFE + MT6358 codec; playback to the bottom speaker via the LoudSPK path, volume through `Lineout Volume`; works after reboot |
| Charger | Richtek RT9470D via the `rt9471` driver: status, current/voltage limits, BC1.2, OTG boost |
| Battery | `mt6358_battery` (v1): voltage + OCV capacity estimate, status from the charger |
| Clock / reset | MT6789 CCU, fixed out-of-bounds in `infra_ao` reset offsets |
| GPU | Panfrost binds the Mali-G57 (`13000000.gpu`) and registers a DRM node; GL/EGL userspace not tested yet |

### Not working

| Subsystem | Notes |
|---|---|
| Top (earpiece) speaker | only the bottom speaker plays; the second amp path (RCV or a digital amp on I2S3) is not wired up yet |
| Amp I2C control | the speaker amp (RT5512/FS1559 at `i2c6 0x5c`) and gate IC (`0x11`) do not ACK; audio works in analog passthrough |
| Battery gauge | no FGADC driver: no current, no coulomb counter; `voltage_now` currently reports `-110` (open regression) |
| Wi-Fi / Bluetooth | on-SoC CONNSYS, vendor 5.10 driver only |
| Modem | vendor stack only |
| Camera | no driver |
| cpuidle / suspend | not implemented, not tested |

## Device tree and DTB

* `arch/arm64/boot/dts/mediatek/mt6789-infinix-x676b-linux.dts` — upstream-style
  DTS (kbuild).
* `arch/arm64/boot/dts/mediatek/mt6789-infinix-x676b-live.dts` — flattened DTS
  actually used on the device (PMIC, LVTS, audio, SD, framebuffer,
  reserved-memory).
* `arch/arm64/boot/dts/mediatek/mt6789-infinix-x676b-live.dtb` — prebuilt DTB
  (v49) matching the `-live.dts` above.

Build the DTB with `dtc` (not kbuild):

```sh
dtc -I dts -O dtb -o mt6789-infinix-x676b-live.dtb mt6789-infinix-x676b-live.dts
```

The DTS comments are in English so the reasoning is clear to other readers.

## Display

The panel is an Ilitek ILI7835 1080x2400 AMOLED in MIPI DSI command mode.
U-Boot leaves the MMSYS pipeline, the DSI controller and the D-PHY
initialized; the kernel only has to push frames.

* **Scanout buffer**: `shared-dma-pool` node `display-fb@feabe000` in
  `reserved-memory` (the middle of the three bootloader framebuffers),
  referenced by `memory-region` in the `display@14000000` node.  The DRM
  buffer therefore lands exactly at `0xfeabe000`, the address the bootloader
  programs into OVL0.
* **Driver**: out-of-tree `x676b_drm`
  (`/workdir/git/heliog99/display/x676b-drm`), built against this tree and
  loaded from `modules-load.d`.  It registers `drm_simple_display_pipe` +
  `drm_gem_dma` + fbdev; `simple-framebuffer` is disabled in the DTB.
* **Visible layer**: OVL0 **layer 1**.  `L1_CON` must be `0x20ff` (per-pixel
  alpha off): fbcon writes XRGB with alpha = 0, so an alpha-enabled layer is
  fully transparent and the screen stays black.
* **Frame trigger**: reprogram OVL0 -> RDMA0 -> DITHER0 -> DSI0 and then send
  DCS `0x2C` with `CMDQ_SIZE=0x8001` plus a `DSI_START` bit-0 toggle, from a
  single periodic thread every 16 ms.  Do not trigger from atomic commits and
  do not write `DSI_INTSTA`.
* **`DSI_CON` bit 0 is the DSI reset**: it must be deasserted after any reset
  pulse.  If it stays set the engine is held in reset: no frames and no DCS
  commands reach the panel (looks like a dead panel until a power cycle).

## SD card (MSDC1)

* Controller `11240000.mmc`, driver `mtk-sd` (`CONFIG_MMC_MTK=m`), host `mmc0`.
* Tested with a 128 GiB SDXC card: `mmc0: new high speed SDXC card at address
  b368`, `mmcblk0: mmc0:b368 SDABC 117 GiB`; 50 MHz clock (48 MHz actual),
  4-bit bus, 3.3 V, SD high-speed timing, `/dev/mmcblk0` and
  `/dev/mmcblk0p1`.
* Card detect is an active-high GPIO routed through EINT (GIC SPI 244), so
  insertion and removal are hotplug events.
* A single `mmc0: error -84 whilst initialising SD card` at boot is the first
  CMD8 attempt; the retry succeeds and the card enumerates normally.
* The same slot is still not usable from U-Boot; the Linux bring-up does not
  depend on it (DTS commit `0611a4104`).

## Building

```sh
export CROSS_COMPILE=aarch64-linux-gnu-
make ARCH=arm64 -j12 Image modules
```

Important configuration choices for this device:

* `CONFIG_LOCALVERSION="-x676b"` and `CONFIG_LOCALVERSION_AUTO=n` — keeps
  the module vermagic stable (`7.0.0-x676b+`) across commits.
* `CONFIG_MMC_MTK=m` — the SD driver is a module, so it can be rebuilt and
  reloaded without a new `Image`.
* `CONFIG_DRM_SIMPLEDRM=m`, the MT6789 AFE/machine/codec drivers are modules
  as well, for the same reason.

## Changes over upstream

* **Sound**: MT6789 AFE driver (`sound/soc/mediatek/mt6789/`), machine driver
  `mt6789-mt6366`, MT6358 codec integration, RT5512 amplifier driver
  (from the 2022-10-13 LKML patch), 18 AFE pin states, bit-banding memif
  support in the common AFE code.
* **Power domains**: MT6789 `pmdomain` table ported from the vendor tree,
  including the AUDIO domain required by the AFE.
* **SD (MSDC1)**: fixed the slot on X676B — drive-strength/pull values,
  real PMIC LDOs for VDD/VccQ, active-high card detect, correct EINT SPI
  (244) so card-detect and PMIC interrupts are delivered.
* **MMC**: `mtk-sd` no longer stores the DMA mask in driver-private memory,
  fixing a KASAN use-after-free on unbind/rebind.
* **Thermal**: `thermal: mediatek: lvts` MT6789 support — 4 controllers,
  13 sensors, filtered mode, vendor calibration handling.
* **Clock / reset**: `infra_ao` reset offset fix (KASAN out-of-bounds),
  audiosys clock driver now populates the AFE child device.
* **DTS**: Infinix X676B device nodes, reserved-memory (`ranges;`),
  `display-fb` DMA pool for the scanout buffer, GPIO hogs for the speaker
  amp power/enable, prebuilt DTB.

## Documentation

* `Documentation/sound/mediatek/mt6789-x676b-audio.md` — audio bring-up
  notes and the AFE/codec/amp wiring.
* `Documentation/sound/mediatek/mt6789-x676b-refs/` — RT5512 LKML patch and
  the DS5512B datasheet.

The wider project notes (display bring-up, U-Boot, USB, storage, every
workaround with its rationale) live outside this kernel tree in
`docs/AGENTS/` and `docs/hacks/`.

## Sources and credits

Nothing here was invented from scratch: the device support is assembled from
the following projects, all GPL-2.0.

| What | Where |
|---|---|
| Base kernel tree | [ellyq/mtk_g99-mainline](https://gitlab.postmarketos.org/ellyq/mtk_g99-mainline) (this fork's upstream) |
| MT6789 LVTS sensor map, calibration offsets and coefficients | [linux-mediatek/linux](https://github.com/linux-mediatek/linux) — `drivers/thermal/mediatek/lvts_thermal.c`, `include/dt-bindings/thermal/mediatek,lvts-thermal.h` (MT6789 patchset), `arch/arm64/boot/dts/mediatek/mt6789.dtsi` |
| Vendor LVTS behaviour (4 controllers, filtered mode, efuse layout, RCK calibration) | [adomerle/android_kernel_mediatek_mt6789](https://github.com/adomerle/android_kernel_mediatek_mt6789), branch `alps-mp-s0.mp1`, files `drivers/thermal/mediatek/soc_temp_lvts.c` and `soc_temp_lvts.h` |
| MT6789 AFE and MT6358 codec behaviour | MediaTek MT6789 downstream sources (`sound/soc/mediatek/mt6789/`, `sound/soc/codecs/mt6358.c`) |
| RT5512 amplifier driver | LKML patch "ASoC: rt5512: add RT5512 codec driver" (2022-10-13), included in `Documentation/sound/mediatek/mt6789-x676b-refs/` |
| SoC register map, PMIC (MT6366), display and UFS bring-up | MediaTek MT6789 downstream sources and device dumps (stock DTB/DTBO, `mt6789.dtsi`, MTK kernel headers) |
| Device-specific nodes (panel, reserved-memory, keys, USB, audio, SD) | Dumped from the stock firmware of the Infinix X676B (DTB/DTBO from the `vendor_boot` partition) |

The MT6789 thermal driver code in this tree is a port of the vendor
`soc_temp_lvts.c` data tables into the mainline `lvts_thermal` driver, adapted
to the mainline calibration API. The MT6789 AFE driver is a trimmed port of the
vendor `mt6789-afe-pcm.c` onto the mainline `mtk-afe-*` framework.

## License

GPL-2.0, same as the Linux kernel. See `COPYING` and `LICENSES/`.
