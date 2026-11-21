# Mainline Linux for Infinix Note 12 Pro (X676B, MediaTek MT6789 / Helio G99)

Fork of [ellyq/mtk_g99-mainline](https://gitlab.postmarketos.org/ellyq/mtk_g99-mainline)
with support for the **Infinix Note 12 Pro (X676B)** added.

Device: MT6789 (Helio G99), 8 GiB RAM, 256 GiB UFS, 1080x2400 AMOLED,
Android 12 stock firmware with U-Boot as the bootloader.

## Status (tested on hardware)

### Works

| Subsystem | Notes |
|---|---|
| Boot | Arch Linux ARM on UFS, systemd |
| RAM | full 8 GiB visible and usable |
| USB | Type-C composite gadget: ACM console + RNDIS network (SSH) |
| Storage | UFS read/write |
| PMIC | MT6366 via pwrap: 40 regulators, RTC, power/reset keys |
| CPU frequency | 2 clusters (6x A55 up to 2.0 GHz, 2x A76 up to 2.2 GHz), schedutil |
| Thermal | MT6789 LVTS, 13 zones, 110 C critical trips |
| Display | simpledrm + small DSI refresh helper (text console on the panel) |
| Clock / reset | MT6789 CCU, fixed out-of-bounds in infra_ao reset offsets |

### Not working

| Subsystem | Notes |
|---|---|
| Battery / charger | MT6375 charger, no mainline driver yet |
| Audio | no MT6789 AFE driver upstream (MT6358 codec only) |
| Wi-Fi / Bluetooth | on-SoC CONNSYS, vendor 5.10 driver only |
| Modem | vendor stack only |
| Camera | no driver |
| GPU | no Mali driver, simpledrm only |
| SD card | MSDC1 enumerates, card does not answer CMD8 |
| cpuidle / suspend | not implemented, not tested |

## Device tree and DTB

* `arch/arm64/boot/dts/mediatek/mt6789-infinix-x676b-linux.dts` — upstream-style
  DTS (kbuild).
* `arch/arm64/boot/dts/mediatek/mt6789-infinix-x676b-live.dts` — flattened DTS
  actually used on the device (PMIC, LVTS, framebuffer, reserved-memory).
* `arch/arm64/boot/dts/mediatek/mt6789-infinix-x676b-live.dtb` — prebuilt DTB
  (v29) matching the `-live.dts` above.

Build the DTB with `dtc` (not kbuild):

```sh
dtc -I dts -O dtb -o mt6789-infinix-x676b-live.dtb mt6789-infinix-x676b-live.dts
```

## Changes over upstream

* `thermal: mediatek: lvts: add MT6789 support` — 4 controllers, 13 sensors,
  filtered mode, vendor calibration handling.
* `clk: mediatek: mt6789: add missing infra_ao reset offset` — fixes a
  global out-of-bounds found with KASAN.
* `arm64: dts: mediatek: add Infinix Note 12 Pro (X676B)` — device nodes and
  prebuilt DTB.

## Sources and credits

Nothing here was invented from scratch: the device support is assembled from
the following projects, all GPL-2.0.

| What | Where |
|---|---|
| Base kernel tree | [ellyq/mtk_g99-mainline](https://gitlab.postmarketos.org/ellyq/mtk_g99-mainline) (this fork's upstream) |
| MT6789 LVTS sensor map, calibration offsets and coefficients | [linux-mediatek/linux](https://github.com/linux-mediatek/linux) — `drivers/thermal/mediatek/lvts_thermal.c`, `include/dt-bindings/thermal/mediatek,lvts-thermal.h` (MT6789 patchset), `arch/arm64/boot/dts/mediatek/mt6789.dtsi` |
| Vendor LVTS behaviour (4 controllers, filtered mode, efuse layout, RCK calibration) | [adomerle/android_kernel_mediatek_mt6789](https://github.com/adomerle/android_kernel_mediatek_mt6789), branch `alps-mp-s0.mp1`, files `drivers/thermal/mediatek/soc_temp_lvts.c` and `soc_temp_lvts.h` |
| SoC register map, PMIC (MT6366), display and UFS bring-up | MediaTek MT6789 downstream sources and device dumps (stock DTB/DTBO, `mt6789.dtsi`, MTK kernel headers) |
| Device-specific nodes (panel, reserved-memory, keys, USB) | Dumped from the stock firmware of the Infinix X676B (DTB/DTBO from the vendor_boot partition) |

The MT6789 thermal driver code in this tree is a port of the vendor
`soc_temp_lvts.c` data tables into the mainline `lvts_thermal` driver, adapted
to the mainline calibration API.

## License

GPL-2.0, same as the Linux kernel. See `COPYING` and `LICENSES/`.
