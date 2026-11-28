# Mainline Linux for Infinix Note 12 Pro (X676B, MediaTek MT6789 / Helio G99)

Fork of [ellyq/mtk_g99-mainline](https://gitlab.postmarketos.org/ellyq/mtk_g99-mainline)
with the X676B device support maintained here:

* repository: `git@github.com:mnnoee/mtk_g99-mainline-infinix-x676b.git`
* branch: `master`
* kernel: `7.0.0-x676b+`, Arch Linux ARM root filesystem

The phone boots from U-Boot (packed into the stock LK partition) with the
kernel and DTB stored as files on the ext4 `tranfs` partition; the root
filesystem is on a microSD card.  USB (ACM + RNDIS) and WiFi provide the
management link.

## Current status (2026-11-28)

### Works (confirmed on hardware)

| Capability | Notes |
|---|---|
| Boot | U-Boot → `Image.x676b` + `x676b.dtb` from ext4 on tranfs; rootfs on microSD; systemd; SSH over USB and WiFi |
| Display | `mediatek-drm` + `mtk_dsi` + `panel-ilitek-ili7835`, 1080×2400, **60.8 fps** (16.45 ms/frame), TE-paced, DSI 1120 Mbit/s/lane, RDMA memory mode, 32bpp |
| Weston | DRM backend + GL renderer (Panfrost), `weston-terminal`, `weston-simple-egl`, 60 fps |
| GPU | Panfrost Mali-G57 MC2, EGL 1.5, OpenGL 3.1, OpenGL ES 3.1 (GLSL ES 3.10) |
| Touch | FocalTech FT5x06 (`edt_ft5x06`), i2c 0-0038 |
| Volume keys | `mt6779-keypad`, row 4 col 0/1 → `KEY_VOLUMEUP` / `KEY_VOLUMEDOWN`; **volume up wakes from suspend** |
| WiFi | MT6631 via WMT/`wlan_gen4m`, 2.4/5 GHz, static `10.0.16.115`, ~60 Mbit/s |
| Bluetooth | `hci_stp` + WMT, `hci0` `C4:64:F2:87:1F:80`, BR/EDR + LE, LE scan, pairing/OBEX |
| USB | Type-C device role, composite ACM + RNDIS gadget, `10.42.0.13` |
| Flash | OCP81375 main/aux channels, torch levels 1–6, off |
| Haptic | AW8622x, timed pulses, stop |
| Battery/charger | `mt6358_battery` (voltage/status/capacity), `rt9471` charger online |
| Thermal | MT6789 LVTS, 13 zones, 34–64 °C under load, no throttling |
| CPU | 6× A55 up to 2.0 GHz, 2× A76 up to 2.2 GHz; 8× load stable |
| RAM | 8 GiB physical, ~6.69 GiB visible after reservations; 1 GiB test passed |
| microSD | rootfs, 100 MB write ≈ 19.8 MB/s, no mmc errors |
| Watchdog/RTC/PMIC | `mtk_wdt`, `mt6397-rtc`, MT6366 PMIC |
| Suspend | s2idle; RTC wake and volume-up wake verified (7 s in the button test) |

### Partial

| Capability | What works | Limitation |
|---|---|---|
| GPU performance | hardware GLES 3.1 | devfreq stays at 390 MHz; glmark2 score 16 |
| Audio | card `mt6789-mt6366`, mixer controls | playback returns `-112 EHOSTDOWN` |
| FM radio | MT6631 tuner, TUNE/RSSI | no antenna/audio path |
| GNSS | `gps_stp`, WMT `FUNC_ON(2)`, `/dev/stpgps` | no fix: needs `mnld`, LNA GPIO25 not in DT |
| USB Type-C | device role, charging | host/OTG role not tested (no adapter) |
| PMIC power key | input device registered | MT6366 key IRQ does not fire (MT6358 vs MT6366 registers) |
| SCP / SensorHub | SCP firmware booted once; AP→SCP control works; IIO reads share-memory ring; synthetic accel/mag/light/prox tested | SCP→AP mailbox IRQ missing; live sensors blocked until SCP can be restarted (needs full power cycle) |
| UFS | read-only use of tranfs for kernel/DTB | no destructive tests by policy; rootfs is on microSD |

### Not working

| Capability | Reason |
|---|---|
| Cameras | no mainline camsys MT6789 driver |
| Modem (MD1) | vendor-only power sequence; `PWR_ACK` not obtained |
| Full SensorHub | SCP→AP IPI path not implemented; host sensorhub stack only partially ported |
| Audio playback | AFE/codec path returns `EHOSTDOWN` |

## Device tree and DTB — one source

The only device tree source is:

```
arch/arm64/boot/dts/mediatek/mt6789-infinix-x676b-linux.dts
```

Build it with kbuild:

```sh
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- dtbs
```

and deploy the result to the board as `/mnt/x676b.dtb`:

```
.../mt6789-infinix-x676b-linux.dtb
```

Do not add `-live`, `-kms` or other parallel DTS variants.  Historical
DTBs are kept only in `work/dtb-archive/` on the host and are not used
for booting.  The single source covers PMIC, keypad, LVTS, audio, SD,
UFS, display/DRM, reserved-memory, WiFi and the (disabled) SCP node.

## Building

```sh
export CROSS_COMPILE=aarch64-linux-gnu-
make ARCH=arm64 -j12 Image modules
```

Important configuration choices:

* `CONFIG_LOCALVERSION="-x676b"` and `CONFIG_LOCALVERSION_AUTO=n` — keeps
  the module vermagic stable (`7.0.0-x676b+`) across commits.
* `CONFIG_MMC_MTK=m` — SD driver is a module.
* `CONFIG_DRM_SIMPLEDRM=m` and the MT6789 AFE/machine/codec drivers are
  modules as well.
* `CONFIG_KEYBOARD_MT6779=m` — volume keypad.
* `CONFIG_MTK_SCP_TINYSYS=m` and `CONFIG_MTK_X676B_SENSORHUB=m` — the
  experimental SCP/SensorHub stack (see below).

On the board `/lib` is a symlink to `/usr/lib`; install modules to
`/usr/lib/modules/7.0.0-x676b+/...` and run `depmod -a`.

## Booting

The board always stops at the U-Boot prompt after a reboot.  From the
host (`/dev/ttyACM0`):

```sh
cd x676b_tools_linux
python3 boot_arch.py          # ext4 kernel/DTB, rootfs on microSD
```

Useful environment overrides: `ADTB=x676b.dtb`, `AIMG=Image.x676b`.
The default DTB on `/mnt` is the consolidated `x676b.dtb`.

## Display

The panel is an Ilitek ILI7835 1080×2400 AMOLED in MIPI DSI command
mode.  The current stack reaches 60.8 fps with TE pacing:

* Pipeline: `mediatek-drm` — OVL0, RDMA0 (memory mode), DITHER0, DSI0,
  `mtk-mmsys`, `mtk-mutex`, MT6789 D-PHY, `clk-mt6789-mm`.
* The panel TE signal paces the frame; `cmd_fast_trigger=1` lets the
  DSI trigger on TE while the next frame is prepared.  DSI runs at
  1120 Mbit/s per lane; `cmd_sync_enable=0` keeps the fbdev buffer
  write-combined.
* RDMA is reprogrammed when the address, pitch or format changes; the
  32bpp mapping follows the vendor table (XRGB8888/ARGB8888 →
  RGBA8888, RGBX/RGBA → ARGB8888).
* Weston uses the DRM backend with the Panfrost GL renderer; 60 fps.

## Keypad

* `mt6779-keypad` at `0x10010000`, 8×9 matrix, 26 MHz `kpd` clock,
  IRQ 106.  Keymap: row 4 col 0 = `KEY_VOLUMEUP`, row 4 col 1 =
  `KEY_VOLUMEDOWN`.
* Both volume keys report press/release; `wakeup-source` makes volume
  up wake the board from s2idle (verified, ~7 s vs a 300 s RTC alarm).
* The PMIC power key is described (`mt6358-keys` + `power` child with
  `KEY_POWER`, `wakeup-source`) but its IRQ does not fire yet: the
  PMIC is an MT6366 and needs its own key registers rather than the
  MT6358 defaults.

## SCP / SensorHub (experimental)

The tinysys SCP is a separate RV55 core; its stock firmware is a
MediaTek container, not an ELF.  Current state:

* `drivers/soc/mediatek/mtk-scp-tinysys.c` — firmware parser, TCM/DRAM
  layout, reset/power, mailbox/IPI transport, reserve-memory API.
* `drivers/misc/mediatek/sensorhub/` — ported vendor SensorHub 2.0
  (`hf_manager`, `ipi_comm`, `sensor_comm`, `sensor_list`,
  `transceiver`, `share_memory`, `timesync`, `debug`, `custom_cmd`).
* SCP boot was achieved once via the ATF SMC release; the logger shows
  the mailbox init, 250 MHz clock, MXC4005X and the transceiver.
  AP→SCP control works.  SCP→AP mailbox IRQ is still missing.
* `drivers/iio/accel/x676b-scp-accel.c` bypasses the missing IPI and
  polls the SCP share-memory ring.  It exposes accel, magnetometer,
  light and proximity; synthetic ring tests pass
  (`in_accel 9800/-268/229`, `in_magn 300/-200/450`,
  `in_illuminance 1234`, `in_proximity 1`).
* Live sensor data requires the SCP to be running; after the mbox
  crash the SCP no longer restarts without a full power cycle.
* Debug: `/sys/kernel/debug/x676b_scp_accel` dumps the sensor list and
  the last ring records.

## Recent X676B work

* `e2ad5b7d2` — keypad + PMIC power key nodes.
* `61bcfe9c1`, `792fbea5d` — removed the stale `-live` DTS/DTB; one
  DTS/DTB source.
* `1cca984fa` — wrapped-ring handling in the IIO driver.
* `d3445c632` — magnetometer, light and proximity channels.
* `f8306e6ab` — accel uses `value[0..2]` and the sensor gain.
* `2705235df` — share-memory accelerometer IIO driver.
* `ed50a2ad6` — debugfs ring/list dump.
* `08fbb4dc7` — vendor mailbox pin packing (send→recv accumulator).
* `f23d2dd9e`, `bdbdd0544` — tinysys SCP loader and awake/SMC release.
* `53b27fa6c` — SensorHub 2.0 transport import.

## Where the numbers and register values come from

* Disassembly of the stock bootloader (`lk_a`), vendor kernel sources
  (`mt6789`, `scp`, `sensorhub`) and device dumps (DTB/DTBO, firmware).
* Upstream MediaTek drivers and bindings for MT8192/MT8183/MT8195 as
  the reference for the SCP/MMSYS/LVTS blocks.
* Hardware tests on the X676B: display timings, thermal zones, PMIC,
  charger, WiFi/BT/FM/GPS, keypad, suspend/resume.

## Sources and credits

| Area | Source |
|---|---|
| Base tree | [ellyq/mtk_g99-mainline](https://gitlab.postmarketos.org/ellyq/mtk_g99-mainline) |
| MT6789 display/MMSYS/DSI | mainline MediaTek DRM, vendor MT6789 sources, X676B dumps |
| LVTS thermal | `linux-mediatek/linux`, `adomerle/android_kernel_mediatek_mt6789` |
| SCP / SensorHub | vendor MT6789 Android 5.10 sources, `re_sensor`/`re_scp` reverse engineering |
| Keypad / PMIC | mainline `mt6779-keypad`, `mtk-pmic-keys`, vendor MT6366 data |

## License

GPL-2.0-only, matching the upstream kernel.
