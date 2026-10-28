# QwzMac / AirportWCN7850: design, bring-up and debugging

A macOS driver for the **Qualcomm WCN7850** Wi-Fi chip (FastConnect 7800, sold as the NCM865 module), the Wi-Fi 7 card on boards such as the MSI MAG X870 Tomahawk WiFi.

It is a port of OpenBSD's [`qwz(4)`](https://man.openbsd.org/qwz.4) driver, running on the 802.11 stack and network controller from [itlwm](https://github.com/OpenIntelWireless/itlwm). Like itlwm, it shows up in macOS as an Ethernet interface; you pick networks with [HeliPort](https://github.com/OpenIntelWireless/HeliPort).

> **Status: working on one machine (2026-10-15).** On an MSI MAG X870 Tomahawk WiFi (Ryzen, macOS 26.7 Tahoe, OpenCore, no OCLP) both builds work: `QwzMac.kext` (Ethernet-style interface + HeliPort) and `AirportWCN7850.kext` (native Wi-Fi: the macOS Wi-Fi menu, private addresses, Apple's own Wi-Fi stack). Scans, WPA2 joins, DHCP and traffic. 802.11n gave about 170 Mbit/s each way; 802.11ac (VHT80, 2 streams) was added on 2026-10-19 and is being tested. The chip's 11ax/11be modes are not driven. See [Bring-up and debugging](#bring-up-and-debugging) and [Testing safely](#testing-safely).

## What to expect

From `qwz(4)` (this is what OpenBSD supports today):

- WPA2-PSK (CCMP) and open networks. WPA1 and WEP use software crypto. No WPA3/SAE, no 802.1X without a supplicant.
- 802.11n and 802.11ac rates (VHT80, up to MCS 9, 2 spatial streams, beamformee). VHT160/80+80 are off, as in Linux ath12k. The chip's Wi-Fi 6/7 features (11ax/11be, 320 MHz, MLO) are not used. `-novht` in boot-args falls back to 802.11n.
  The 11ac support is macOS-only: OpenBSD's net80211 has no VHT station mode, itlwm's does. The driver advertises the firmware's VHT capabilities, sends the negotiated VHT state in the WMI peer-assoc command (`qwz_peer_assoc_h_vht`, from ath12k) and restarts the vdev on the 80 MHz channel after association, because the width is only known from the association response.
- Broadcast and multicast are only received on open networks or WPA2 with a CCMP group cipher, so ARP and IPv6 can fail on TKIP-group networks.

From the macOS side:

- `QwzMac.kext`: no native Wi-Fi menu, AirDrop, Handoff or Continuity; the interface is Ethernet, as with itlwm. `AirportWCN7850.kext`: native Wi-Fi menu and settings through Apple's IO80211Family (laobamac's AirportItlwm "WCL" layer, as reshaped by SimpleMtkWlan), macOS 26 only, no OCLP; AirDrop/AWDL and WPA3-only networks still unsupported (AWDL is not coming: see [docs/awdl-feasibility.md](docs/awdl-feasibility.md)). Use one or the other: both match the card.
- Only one PCIe interrupt vector is used for now. The chip supports 32, but one is enough for qwz, which has a single-vector mode.

## How it works

| Part | Where | Notes |
|---|---|---|
| WCN7850 driver | `qwz/qwz.c`, `qwz/if_qwz_pci.c` | OpenBSD `qwz(4)`, compiled as C++. Changes from upstream are mechanical: mbuf field access becomes mbuf KPI calls, and C's implicit conversions get casts (`scripts/port/`). |
| OpenBSD kernel API | `qwz/qwz_compat.{h,cpp}` | Sleep/wakeup, locks, PCI, interrupts, DMA and firmware loading on IOKit. The header explains the concurrency and DMA model. |
| 802.11 stack | `itl80211/` | itlwm's port of OpenBSD net80211. One change: task threads run holding the controller's gate. |
| Controller + HAL | `controller/` | itlwm's `IOEthernetController` and HeliPort user client, with `ItlQwz` as the only HAL. |
| Firmware | `firmware/` | Fetched from linux-firmware at build time (not committed), embedded in the kext. |

Two things differ from OpenBSD and are handled in `qwz_compat`:

- **Locking.** The IOKit workloop's command gate plays the role of OpenBSD's kernel lock. `tsleep` releases the gate while it sleeps, the way OpenBSD's `tsleep` releases the kernel lock, so interrupts keep arriving while the driver waits on the firmware.
- **DMA.** The WCN7850 only reaches the first 4 GB of memory. OpenBSD/amd64 keeps every mbuf there; macOS does not. All DMA memory is allocated below 4 GB, and packet buffers are copied through pooled low-memory bounce buffers.

## Building

Needs the Xcode Command Line Tools (no Xcode), git, curl and python3.

```bash
scripts/fetch-firmware.sh
```

```bash
./build.sh
```

`build/QwzMac.kext` is the result. For a build that prints qwz's internal trace:

```bash
DEBUG=1 ./build.sh "$PWD/build-debug"
```

The first build clones [MacKernelSDK](https://github.com/acidanthera/MacKernelSDK) into `deps/`. Firmware comes from linux-firmware release `20260309`, the release OpenBSD's qwz firmware package uses; `firmware/SHA256SUMS` pins the files.

## Bring-up and debugging

The bring-up runs at boot. `qwz_stage=<n>` in boot-args stops it before stage *n* and leaves the driver loaded and idle; `-qwzoff` disables the driver. Stages: 1 task queue, 2 gate, 3 attach, 4 config/BAR, 5 MSI, 6 D0 + first registers, 7 DMA, 8 attach done, 9 chip reset, 10 MHI, 11 firmware download, 12 MHI channels, 13 powered up, 14 QMI done, 15 CE/DP rings, 16 WMI ready, 17 core started, 18 pdev + mac, 19 init done. Stages 14–19 park (firmware left running, interrupts off) instead of cleaning up; reboot before another attempt.

From the terminal, with the driver idle:

| Command | What it does |
|---|---|
| `scripts/qwz-step.sh <n>` | `sysctl debug.qwz.stage=<n>` + `debug.qwz.start=1`: run the bring-up up to stage *n* and print the result |
| `scripts/qwz-log.sh [lines]` | republish and print the in-memory log ring (`QwzMacLog` on the PCI device in the I/O Registry) |
| `sysctl debug.qwz.debug=<mask>` | qwz trace mask (`QWZ_D_*` in `qwz/qwzvar.h`; 0x1 misc, 0x8 WMI, 0x40 MAC, 0x80 mgmt) |
| `sysctl debug.qwz.logfile=1` | also append every line to `/private/var/log/qwzmac.log`, fully synced, so the last line survives a hard freeze |
| `sysctl debug.qwz.he=0` | no 802.11ax for the next association (default 1). It can only turn HE off: HE exists at all only with the `itlwm_he=1` boot-arg (AirportWCN7850.kext) |
| `sudo dmesg \| grep qwz` | the same lines from the kernel buffer |

The `debug.qwz.*` sysctls are registered by both kexts (`qwz/qwz_sysctl.cpp`), so `scripts/qwz-log.sh`, `debug.qwz.logfile` and the trace mask also work with `AirportWCN7850.kext`. Only `debug.qwz.start` is QwzMac-only: the Airport product has no idle bring-up to rerun and answers `ENXIO`.

With `AirportWCN7850.kext` the ring also records the first request of every apple80211 ioctl id that reaches the driver (`airport: ioctl 195 (APPLE80211_IOC_MCS_VHT) first asked of the driver`) and the first calls of the link getters (`airport: getMCS_VHT mcs 5 nss 2 gi 400 ...`); `scripts/qwz-log.sh 400 | grep airport` after `sudo wdutil info` shows which values macOS asked for and what was answered. IO80211Family answers many requests from its WCL layer first (noise and CCA only ever come from the link-quality snapshot the driver posts) and asks the driver only for the rest.

`scripts/set-usb-stage.sh <n>` deploys `build-debug/QwzMac.kext` to the USB EFI, sets `qwz_stage=<n>` (and `qwz_debug` with `QWZ_DEBUG=<mask>`), unmounts and checks the filesystem. Messages from an OpenCore-injected kext never reach the unified log, which is why the ring and the file exist.

Bugs found on the way, all macOS-specific and fixed: a `#define explicit_bzero bzero` in net80211 turned the compat `explicit_bzero` into a self-recursive `bzero` (every bring-up froze); `IOCommandGate::runAction` passes the gate's owner, not the caller (NULL softc after attach); xnu's `mbuf_pullup` always splits a cluster mbuf (WMI events unparseable); the bounce layer skipped the copy-out after a PREREAD (HTC never connected); the CE interrupt mask was never set with one MSI vector; the vdev must be created on the first interface-up when the firmware is left running.

## Testing safely

A kernel driver bug panics the whole machine, and this one has not run on hardware yet. Before installing:

1. **Keep a way back.** A known-good OpenCore EFI, and remember that **Safe Mode** (hold Shift while choosing macOS in the OpenCore picker) does not load third-party kexts.
2. **Add a delay** so a crash during bring-up happens after you have logged in:

   ```bash
   sudo nvram boot-args="$(nvram -p | awk -F'\t' '/^boot-args/{print $2}') qwz_delay=60"
   ```

   The driver waits 60 seconds before touching the card. If it panics, you then have time after the next login to switch it off:

   ```bash
   sudo nvram boot-args="$(nvram -p | awk -F'\t' '/^boot-args/{print $2}') -qwzoff"
   ```

3. **Don't let the Mac sleep** during the first tests: sleep/wake is wired in AirportWCN7850 (chip power-down on sleep, link/config recovery and full firmware re-init on wake; boot-arg -qwznowakefix restores the old behaviour); QwzMac still has no sleep support.
4. Do not install alongside **itlwm**: both register a class named `itlwm` (that is how HeliPort finds the driver).

### Install

```bash
sudo cp -R build-debug/QwzMac.kext /Library/Extensions/
```

```bash
sudo chown -R root:wheel /Library/Extensions/QwzMac.kext
```

```bash
sudo kmutil load -p /Library/Extensions/QwzMac.kext
```

Approve it in System Settings if asked, then reboot.

If you inject kexts with OpenCore instead, add `QwzMac.kext` to `EFI/OC/Kexts` and `Kernel → Add`.

Also disable `SSDT-Disable_Network_GPP7.aml` if your config has it: it is meant to hide this card from macOS.

### What to look at

```bash
log show --last boot --predicate 'eventMessage CONTAINS "qwz"' --style compact
```

A good bring-up prints the BAR and interrupt being set up, the firmware loading, and finally `attached: MAC xx:xx:..., firmware WLAN.HMT...`. Then a new Ethernet interface appears; join a network with HeliPort.

Debug builds take `qwz_debug=<mask>` in boot-args (bits from `qwz/qwzvar.h`: `0x1` misc, `0x2` MHI, `0x4` QMI, `0x8` WMI, `0x10` HTC, `0x80` management, ...). If the firmware crashes, a debug build attaches its memory dump to the controller as the `QwzRDDM` IORegistry property.

### Removing it

```bash
sudo rm -rf /Library/Extensions/QwzMac.kext
```

```bash
sudo kmutil clear-staging
```

Then reboot.

## Updating from upstream

The first commit holds the upstream files unmodified (see `UPSTREAM.md`), so `git diff <first commit> -- qwz/` shows every local change to qwz. To pick up newer OpenBSD work, diff the new upstream files against that commit and apply the changes, or redo the import on a branch and merge.

## Licenses and credits

- `qwz/qwz.c`, `qwz/if_qwz_pci.c`, `qwz/qwzreg.h`, `qwz/qwzvar.h`: OpenBSD `qwz(4)` (Stefan Sperling and the OpenBSD developers), ISC license, with code derived from the Linux `ath12k` driver under the Linux Foundation / Qualcomm BSD license. See the file headers.
- `itl80211/`, `include/`, `controller/` (except `ItlQwz.*`): itlwm by zxystd, GPL-2.0-or-later, with OpenBSD net80211 under its own BSD/ISC licenses.
- `qwz/qwz_compat.*`, `controller/ItlQwz.*`, `scripts/`: GPL-2.0-or-later.
- WCN7850 firmware: Qualcomm, redistributable in binary form under `LICENSE.QualcommAtheros_ath10k` (fetched with the firmware, not stored here).

The whole kext is distributed under GPL-2.0-or-later (`LICENSE`).
