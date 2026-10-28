# AirportWCN7850: Tahoe fixes fork

This branch (`wcn7850-tahoe-fixes`) builds on razvanccr's AirportWCN7850 0.0.2. Each commit is one build that was run on real hardware; the commit messages say what changed and what was measured.

**Test machine:** MSI MAG X870E Tomahawk WiFi (onboard Qualcomm WCN7850, PCI `17cb:1107`), Ryzen 7 9700X, macOS Tahoe 26.7.1 (25G241), OpenCore 1.0.8, SMBIOS MacPro7,1, 802.11ac access point.

## Status

| Area | Status | Notes |
|---|---|---|
| Signal (RSSI), noise, rate in the Wi-Fi menu, System Settings, `system_profiler` | **Works** | Signed dBm, -95 dBm noise floor, MCS-table rate fallback; RSSI averaged for the connected network |
| Signal bars | **Works** | macOS bars are RSSI-only (3 bars above -70 dBm, 2 down to -84, 1 below) |
| WPA2-Personal, WPA3 transition networks | **Works** | PMF on in peer association; transition networks join on the WPA2 side |
| System sleep and wake | **Works** | Chip powered down for S3 (1.2 s), firmware reloaded on wake (0.8 s); `-qwznowakefix` restores the old behaviour |
| "AirDrop: Supported" in System Information | **Works** | Card capability 32; AWDL interface requests are logged and declined |
| Wi-Fi 6 (802.11ax) | **Built, untested** | Off unless `itlwm_he=1`; no 802.11ax access point was available. `sysctl debug.qwz.he=0` switches it off per association |
| WPA3-SAE-only networks | **Does not work** | `sae/` is unfinished work in progress, not built, does not compile. macOS passes a PMK for WPA2 (cipher 6); what it passes for SAE is not confirmed yet |
| AirDrop, AWDL | **Does not work** | No Tahoe AWDL interface; in station mode the firmware passes no AWDL frames to the host (probe: 0 frames on channels 36 and 149) |
| 6 GHz, Wi-Fi 7 (EHT), MLO | **Not implemented** | |

## Configuration this was tested with

- OpenCore `DeviceProperties` for the Wi-Fi function: `pci-aspm-default = 0` (number). Without it IOPCIFamily enables ASPM L1 and the link drops during idle and display sleep (the driver already avoids ASPM with a single MSI vector).
- boot-args: `itlwm_cc=ZA` (country code), plus `itlwm_he=1` only to try Wi-Fi 6.
- BIOS: ErP enabled (cuts standby power to the module in S3, which is why the wake path reloads the firmware), Resume by PCI-E device disabled.

## Logs

Every new code path writes to the driver's log ring:

```zsh
sudo sysctl debug.qwz.publish=1 >/dev/null; ioreg -l -w0 -r -k QwzMacLog | sed -n '/"QwzMacLog" = "/,/^"$/p' | grep -E 'qwz: pm:|802\.11ax|HE assoc|rx-probe summary|WCL join'
```

- `qwz: pm:` sleep and wake recovery steps, ending in `wake: SUCCESS` or `FAILED`
- `802.11ax:` / `HE assoc:` Wi-Fi 6 capabilities and associations
- `rx-probe summary` once a minute: frame counters and RSSI views
- `WCL join raw` / `setCIPHER_KEY` key types and lengths for each join (never key material)

## Related

Bluetooth on the same chip: the BluetoothWCN7850 fork, branch `ble-hid-pairing`, adds a BlueToolFixup patch (`-btlfxlemask`) without which Bluetooth LE game controllers, mice and keyboards connect but never pair.
