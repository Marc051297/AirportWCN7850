Tahoe fixes on top of razvanccr's AirportWCN7850 0.0.2: sleep/wake and Wi-Fi off/on that bring the chip back, a correct signal display, PMF, and Wi-Fi 6 behind a boot-arg.

**AirportWCN7850-0.0.2-tahoe.5.zip** contains the kext (firmware embedded) and the licenses; nothing else is needed. It replaces the 0.0.2 kext: same bundle identifier (`com.razvanccr.AirportWCN7850`) and version 0.0.2; the Wi-Fi firmware version in System Information reads `itlwm: 0.0.2ae55e33 fw: …`. Install as for 0.0.2 (copy to `EFI/OC/Kexts`, `Kernel → Add` with `MinKernel` 25.0.0, reboot).

Recommended configuration
- OpenCore `DeviceProperties` for the Wi-Fi function: `pci-aspm-default = 0` (number). Without it the link drops during idle and display sleep.
- boot-args: `itlwm_cc=XX` with your country code.
- BIOS: ErP enabled, Resume by PCI-E device disabled. If every sleep wakes again within seconds (seen on an MSI X870E board), set `Wake Up Event By = BIOS` with Resume by USB, PCI-E and RTC disabled; wake with the power button.

Changes since 0.0.2

Sleep, wake and Wi-Fi off/on
- System sleep powers the chip down (1.2 s); wake recovers the PCIe link and config space and reloads the firmware (0.8 s). Before, the driver kept using a dead firmware after wake.
- Before every firmware restart the ring pointers shared with the chip and the firmware's host memory are reset, as the OpenBSD and Linux drivers do. Without this, Wi-Fi died after some wakes (associations with no data, or `tx credits timeout`) until a reboot.
- One owner for each chip transition: the system's power thread, airportd's Wi-Fi on request and the retries never run the recovery at the same time. A Wi-Fi on request that arrives before the system wake reaches the driver waits for it.
- Before the power-down the driver waits up to 3 s for the association teardown to finish.
- A failed recovery is retried after 5 s, 30 s, 60 s, then every 5 minutes (12 tries).
- Wi-Fi off/on is a full chip reset and firmware reload, the same path as sleep.
- Data-stall restart: 10 s associated without one data frame, while management frames arrive, restarts the firmware (at most twice, 60 s apart). It has not triggered in testing yet.

Link display and security
- The Wi-Fi menu, System Settings and `system_profiler` show the real signal (signed dBm), a -95 dBm noise floor and a rate; the RSSI of the connected network is averaged, so the bars stop flickering.
- PMF is on in peer association; WPA3 transition networks join on the WPA2 side.
- System Information shows "AirDrop: Supported" (card capability 32); macOS's AWDL interface requests are logged and declined. AirDrop itself does not work.
- Wi-Fi 6 (802.11ax) station support, off unless `itlwm_he=1`. Untested: no 802.11ax access point was available.

Boot-args
| boot-arg | effect |
|---|---|
| `-qwznowakefix` | old sleep/wake handlers (interface down/up only) |
| `-qwznoradioreset` | Wi-Fi off stops the interface without the chip reset |
| `-qwznostallfix` | no data-stall restart |
| `-qwzstallsbr` | the second data-stall restart also resets the PCIe link |
| `itlwm_he=1` | Wi-Fi 6 (802.11ax) |

Tested on an MSI MAG X870E Tomahawk WiFi (onboard WCN7850), Ryzen 7 9700X, macOS 26.7.1 (25G241), OpenCore 1.0.8, SMBIOS MacPro7,1, 802.11ac access point. This build: Wi-Fi off/on twice and one sleep/wake, all three recovered in about 0.8 s with traffic flowing. An earlier build failed after a number of wakes (once at the 15th) for the reason fixed here; longer runs with this build are still in progress.

Still not: WPA3-SAE-only networks, AirDrop/AWDL and Continuity, 6 GHz, Wi-Fi 7 (EHT) and MLO.

Logs (sleep/wake steps end in `wake: SUCCESS` or `FAILED`):
```zsh
sudo sysctl debug.qwz.publish=1 >/dev/null; ioreg -l -w0 -r -k QwzMacLog | sed -n '/"QwzMacLog" = "/,/^"$/p' | grep -E 'qwz: pm:|802\.11ax|HE assoc|rx-probe summary|WCL join'
```

Bluetooth for the same chip: https://github.com/razvanccr/BluetoothWCN7850 (the `ble-hid-pairing` branch of https://github.com/Marc051297/BluetoothWCN7850 adds a BlueToolFixup patch for Bluetooth LE controllers, mice and keyboards).
