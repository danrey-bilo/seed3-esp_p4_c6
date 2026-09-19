# P4 Wi-Fi settings service

ESP-IDF 5.5.5 component for the Waveshare P4/C6 SDIO board.
`p4_wifi_settings_init()` starts one serialized worker; public requests
`scan`, `connect`, `disconnect`, `set_reconnect` return immediately with
false on validation/queue failure. `get_status` copies a guarded snapshot.

No LVGL dependency, no network calls in the USB/audio ISR, no credentials in
console diagnostics. Successful networks and auto-reconnect are stored in
the `p4_wifi` NVS namespace. RAM credentials are required for reconnect and
NVS is currently **unencrypted**; do not treat this firmware as a secure vault.

The four-entry command queue carries copied strings. Only the worker calls
the remote Wi-Fi API. Event callbacks publish bits; the worker handles DHCP,
disconnect, asynchronous scan completion and bounded retry intervals.
Scan and association have timeouts. Manual disconnect suppresses retries
until another manual connection (or a reboot with auto-connect enabled).

Upstream ESP-Hosted 1.4.7 has startup transport initialization and priority-23
transport tasks. Its memory/traffic footprint must be tested alongside audio.
Settings worker priority 2 does not lower those upstream priorities.

[User instructions and hardware mapping](../../../docs/WIFI_SETTINGS.md)
