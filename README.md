# Mainline Linux for Infinix Note 12 Pro (X676B, MediaTek MT6789 / Helio G99)

Fork of [ellyq/mtk_g99-mainline](https://gitlab.postmarketos.org/ellyq/mtk_g99-mainline)
with support for the **Infinix Note 12 Pro (X676B)** added.

Device: MT6789 (Helio G99), 8 GiB RAM, 256 GiB UFS, 1080x2400 AMOLED.
Bootloader: U-Boot packed into the stock LK partition (separate tree,
`u-boot-mtk-g99` / `x676b_tools_linux`). Root filesystem: a Linux
distribution on a UFS partition.

## Status (tested on hardware)

### Works

| Kernel capability | Notes |
|---|---|
| Boot from UFS | root filesystem on a UFS partition (`root=PARTUUID=...`); a second UFS partition works as an alternative root |
| RAM | full 8 GiB across two banks |
| UFS | block device, read/write |
| SD card (MSDC1) | `mmc0`: SDXC, 50 MHz / 4-bit, card-detect hotplug (insert and remove) |
| USB | Type-C composite gadget (ACM console + RNDIS) and OTG host with role switch |
| PMIC | MT6366 via pwrap: regulators, RTC, power/reset keys, ADC |
| Charger / battery | RT9471 charger driver: status, current/voltage limits, BC1.2, OTG boost; `mt6358_battery` (voltage + OCV estimate) |
| CPU frequency | two clusters: 6x A55 up to 2.0 GHz, 2x A76 up to 2.2 GHz, schedutil |
| Thermal | MT6789 LVTS, 13 zones, 110 C critical trips |
| Display | `mediatek-drm` + `mtk_dsi` + `panel-ilitek-ili7835`: 1080x2400 AMOLED, command mode, RDMA memory mode, 33 ms tick (~30 fps); fbdev emulation on `/dev/fb0` |
| GPU | Panfrost binds Mali-G57 MC2: GLES 3.1 feature set, ~135 GFLOPS FP32 at 1.1 GHz; no Vulkan (PanVK does not support this GPU) |
| Audio | MT6789 AFE + MT6358/MT6366 codec: playback to the bottom speaker (LoudSPK path) |
| Wi-Fi | CONNSYS MT6631 with the ported gen4m driver: scan on 2.4/5 GHz, WPA2-PSK/SAE, ~48 Mbit/s |
| Bluetooth | `hci_stp` (HCI over WMT/STP): BR/EDR and LE, pairing, RFCOMM/L2CAP sockets |
| FM radio | MT6631 tuner via WMT/STP: power-up, tuning, RSSI, band scan |
| GNSS | `gps_stp` STP transport (L1): NMEA at 1 Hz, satellites and C/N0 visible |
| KASAN | the tree doubles as a KASAN bring-up target: several real bugs were found and fixed here (see "Bugs found with KASAN") |

### Partial / not working

| Kernel capability | Notes |
|---|---|
| Top (earpiece) speaker | only the bottom speaker path is wired up |
| Speaker amp I2C | RT5512/FS1559 at `i2c6 0x5c` and the gate IC `0x11` do not ACK; audio works in analog passthrough |
| Battery gauge | no FGADC driver: no current or coulomb counter, capacity is a voltage/OCV estimate |
| Modem | vendor-only stack, not started from mainline |
| Camera | no driver |
| Touchscreen / local input | no driver yet |
| cpuidle / suspend | not implemented, not tested |
| FM / BT audio | audio routing for those profiles is missing |
| 60 Hz display | the 1006 Mbit/s D-PHY link has no margin for a 16.6 ms frame |

## Where the numbers and register values come from

Every constant in this tree has a traceable origin; none of it was invented:

* **Disassembly of the stock bootloader.** The `lk_a` partition was dumped
  from the device and disassembled with Ghidra.
  The DSI command sequences, OVL/RDMA/mutex programming, the panel init
  table, EMI MPU encoding and the display clock/power gates come from that
  code.
* **Disassembly of the ATF/TEE image.** The `tee_a` dump was analysed for
  the EMI MPU register encoding, the SMC entry points and the secure power
  domain handling.
* **A live Android system on the same phone.** Registers, clocks, power
  domains, DSI state and the Wi-Fi/BT/FM/GPS bring-up were read and
  cross-checked on the running stock firmware (sysfs, debugfs, `/proc`,
  logcat, WMT/connsys logs), and used as the reference behaviour.
* **Vendor kernel sources (MediaTek BSP 5.10).** Driver behaviour and data
  tables were ported from the public MT6789 downstream trees: MMSYS
  routing, pm-domains, clocks, AFE, LVTS, CONNSYS/WMT, gen4m, fmradio and
  the GPS STP transport.
* **Device dumps.** DTB/DTBO from `vendor_boot`, firmware images from
  `/vendor/firmware` and partition dumps give the device-specific nodes,
  firmware blobs and the reserved-memory layout.
* **Upstream mainline.** Base drivers, DT bindings and subsystem APIs come
  from mainline; where MT6789-specific data was missing it was filled in
  from the sources above.
* **Measurements on this board.** Timing and performance numbers (30 fps
  tick, 5.7 ms frame conversion, 135 GFLOPS, 48 Mbit/s, thermal behaviour)
  were measured on the device, not taken from a datasheet.

## Device tree and DTB

* `arch/arm64/boot/dts/mediatek/mt6789-infinix-x676b-linux.dts` — upstream-style
  DTS (kbuild).
* `arch/arm64/boot/dts/mediatek/mt6789-infinix-x676b-live.dts` — flattened DTS
  actually used on the device (PMIC, LVTS, audio, SD, display/DRM,
  reserved-memory).
* `arch/arm64/boot/dts/mediatek/mt6789-infinix-x676b-live.dtb` — the prebuilt
  DTB that boots and works with this tree (the same blob the board loads);
  `dtc` on the `-live.dts` above reproduces it byte for byte.

Build the DTB with `dtc` (not kbuild):

```sh
dtc -I dts -O dtb -o mt6789-infinix-x676b-live.dtb mt6789-infinix-x676b-live.dts
```

The DTS comments are in English so the reasoning is clear to other readers.

## Display

The panel is an Ilitek ILI7835 1080x2400 AMOLED in MIPI DSI command mode.
The kernel drives it through the mainline MediaTek DRM stack; the panel
driver (`drivers/gpu/drm/panel/panel-ilitek-ili7835.c`) is part of this
tree.

* **Pipeline**: `mediatek-drm` with the MT6789 components —
  OVL0, RDMA0 (memory mode), DITHER0, DSI0 — plus `mtk-mmsys`,
  `mtk-mutex`, the MT6789 D-PHY and `clk-mt6789-mm`.  Routing lives in
  `drivers/soc/mediatek/mt6789-mmsys.h`.
* **Command-mode tick**: the panel has no hardware vblank, so
  `mtk_crtc_cmd_mode_work()` converts the fbcon XRGB8888 buffer to
  packed RGB888 (NEON), hands it to RDMA in memory mode and sends DCS
  `0x2C` once per period (`cmd_period_ms`, 33 ms by default).
* **Buffers**: MTK GEM buffers and the conversion buffer are cacheable
  non-coherent DMA memory (`dma_alloc_noncoherent()`), so the CPU
  conversion costs ~5.7 ms per frame (~14% of one core at 30 fps).
  Write-combined memory made the fb reads 8x slower.
* **Panel driver**: ILI7835 init sequence, reset/bias GPIOs (85/64),
  DCS `0x11`/`0x29` and `0x51` brightness via the backlight device.
* **Not used**: the direct OVL to RDMA path (`cmd_memory_mode=0`) does
  not survive repeated software triggers without GCE/TE, so frames go
  through RDMA memory mode.

See `Documentation/x676b.rst` for the driver/parameter details and the
known limitations (60 Hz does not fit the 1006 Mbit/s D-PHY link yet).

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

* **Display**: MT6789 MMSYS routing and mutex tables, DSI command-mode
  quirks (`reg_cmdq_off=0xd00`, PSCTRL, CMD_DONE polling), the ILI7835
  panel driver, RDMA memory-mode support, the command-mode frame tick and
  cacheable GEM buffers.
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
* **Connectivity**: out-of-tree ports of the vendor CONNSYS stack used by
  this device — WMT/STP, BTIF, gen4m Wi-Fi, `hci_stp` (HCI over STP) and
  `gps_stp` — plus the vendor `mnld`/fmradio userspace.

## Bugs found with KASAN

* **Display command-mode tick deadlock.** `cancel_delayed_work_sync()` in
  `atomic_disable()` against the tick that takes `plane->mutex`: any
  `fb_blank` (console blanking or a fbdev client) froze a process in D
  state. The tick
  now stops with a flag and a non-blocking cancel.
* **`mtk-mutex` NULL dereference.** `of_find_device_by_node()` returns the
  platform device before its driver probes, so `dev_get_drvdata()` could be
  NULL during `mtk_drm_bind()`; an intermittent boot panic. Now
  `-EPROBE_DEFER` until the data is there.
* **`pinctrl-mt6789` out-of-bounds read.** `mt6789_pull_type[]` had 188
  entries for 222 pins, so `mtk_pinconf_bias_get_combo()` read past the
  array for GPIO188..221. The table is complete now.

## Documentation

* `Documentation/x676b.rst` — X676B kernel support: device tree, display
  drivers, module parameters, known limitations.
* `Documentation/sound/mediatek/mt6789-x676b-audio.md` — audio bring-up
  notes and the AFE/codec/amp wiring.
* `Documentation/sound/mediatek/mt6789-x676b-refs/` — RT5512 LKML patch and
  the DS5512B datasheet.

The wider project notes (display bring-up, USB, storage, every workaround
with its rationale) live outside this kernel tree in `docs/AGENTS/` and
`docs/hacks/`.

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
