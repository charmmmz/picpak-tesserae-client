# PicPak Bluetooth maintenance

Development implementation, compatible with Tesserae Companion BLE protocol v2.
The GATT transport and QR crypto are adapted from
[Tesserae device firmware](https://github.com/dmellok/tesserae-device-firmware),
under AGPL-3.0-or-later. The vendored QR generator retains its MIT license;
the bitmap font is public domain.

## Entering maintenance

While the display is sleeping, press and hold its wake button. At about three
seconds the LED pulses: release before five seconds for Bluetooth maintenance.
Keep holding to five seconds for the existing refresh action, or twenty seconds
for AP setup. The gesture is classified on release, so the longer actions do
not open BLE on their way through three seconds. This first implementation
recognizes gestures at boot/wake; it does not interrupt a running screen refresh
or captive portal. A fresh device still uses the AP for initial server setup.

The display paints a QR code and passkey before advertising. Open Bluetooth
Maintenance in Companion, select the nearby PicPak, then scan the QR code or use
the six-digit passkey. The five-minute deadline starts after screen rendering.
Closing the app disconnects the phone; the same physical session can be rejoined
until that deadline. Deep sleep never advertises. Low-battery protection still
applies to entry.

Basic diagnostics and refresh-speed changes do not start Wi-Fi. Network scanning
and repair are explicit actions. Wi-Fi repair tests association and DHCP, then
saves only Wi-Fi credentials; it does not contact Tesserae or replace REST,
MQTT, relay, or server-registration settings. Clearing Wi-Fi opens one recovery
session after restarting, then falls back to AP setup if it times out. Factory
Reset clears stored application settings and returns to AP setup. Neither reset
operation reflashes the firmware.

The normal image deduplication references are cleared when painting the QR so
the next successful network cycle restores the photo on every transport.
An expired session clears its QR from the screen before normal operation resumes.

## Refresh speed

The Display section exposes `Refresh Speed`: `5 s`, `10 s`, `Native`. These map
to the existing NVS `state/waveform` values 0, 1, 2 without changing the panel's
drivers or waveforms. They control redraw duration, not the schedule between
updates. Faster modes trade colour margin for speed; Native uses the panel's
built-in waveform. Approximate duration depends on the panel and conditions.
Saving does not force an extra refresh; it applies at the next `epd_init()`.

## Wire contract

- Protocol major: **2**; hardware code: **11**; model: **`picpak_4_2`**.
- Service UUID: `7A5E0001-7B6D-4F8B-9C2E-1D0A5A110001`.
- Info / QR control / passkey control / events use UUID suffixes 0002–0005.
- Advertisement is the existing v2 ten-byte service payload: version, mode
  (`0x02` maintenance), hardware, last three MAC bytes, four-byte session ID.
- Device ID is `picpak-` plus the Wi-Fi MAC, independent of server registration.
- QR: `tesserae://setup?v=2&id=<id>&sid=<8-hex>&key=<base64url-32-byte-secret>`.
- Each connection supplies a new 16-byte `connection_nonce` in the info JSON.
  QR frames use HKDF-SHA256 and AES-256-GCM with the existing v2 domain, nonce,
  directional counters, and chunk headers. Native passkey writes require an
  authenticated encrypted LE Secure Connections link; legacy pairing is disabled.
- Messages are limited to 512 bytes. Events and commands preserve FIFO order.
  Both queues are scoped to the originating connection generation; native
  event notifications and reads require the authenticated encrypted link.

Authenticated commands:

| Command | Result |
| --- | --- |
| `{"op":"diagnostics"}` | Existing diagnostics event plus `"refresh_speed":"5s"` (or `10s`, `native`) |
| `{"op":"set_refresh_speed","value":"10s"}` | `{"event":"refresh_speed","value":"10s"}` after successful NVS commit |
| `{"op":"scan"}` | `scan_started`, zero or more `network` events, `scan_complete` |
| `{"op":"stage","ssid":"…","password":"…","preserve_server":true}` | `staged`; credentials stay in RAM |
| `{"op":"apply"}` | `testing_wifi`, `wifi_connected`, then `configured` and restart |
| `{"op":"reboot"}` | `rebooting` and restart |
| `{"op":"clear_wifi"}` | `clearing_wifi` and restart into BLE recovery |
| `{"op":"factory_reset"}` | `factory_resetting` and restart into AP setup |

Unknown speeds and failed writes return `error` with a message, never a success
acknowledgement. Firmware serializes writes and following diagnostic readbacks.
Companion shows the setting only for PicPak when diagnostics includes a known
speed, retains the confirmed value while saving, and reads back after timeout.
Stage accepts omitted or empty `server_url`/`pairing_code` for compatibility;
nonempty values are rejected. Changing servers remains in AP setup.

## Building and checking

The existing 16 MiB flash layout and 2 MiB application slot are retained. No OTA
migration, bootloader change, or partition-table replacement is required.

With PlatformIO installed, run `tools/build_firmware.sh`. It builds a disposable
copy under `/tmp` because ESP-IDF rejects paths containing spaces. Override `PIO`
or `PICPAK_BUILD_DIR` as needed. The script prints the application-only binary path
and checks that it fits the original slot. A normal ESP-IDF build from `firmware/`
is also supported with its `sdkconfig.defaults` and existing partition table.

Run `tools/test_host.sh` for gesture boundaries, settings persistence/error paths,
protocol golden vectors, QR framebuffer bounds, and existing pure firmware tests.
`IDF_PATH` may point to ESP-IDF for the host mbedTLS dependencies;
`PICPAK_TEST_SCREEN` optionally exports the exact test QR framebuffer.

Before distributing: validate QR and passkey connections, reconnects, all three
speed settings after power cycling, Wi-Fi failure with credentials preserved,
low-battery entry, five-minute timeout and sleep current on a physical PicPak.
Host and simulator tests do not establish those hardware results.
