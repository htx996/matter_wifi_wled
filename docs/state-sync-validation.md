# ESP32-S3 state synchronization validation

Experimental branch; do not merge or flash production devices before validation.

## Build

- `pio run -e esp32s3` (8 MB flash)
- `pio run -e esp32s3_16mb` (16 MB flash)
- The existing CI workflow currently builds esp32s3 and classic esp32, not esp32s3_16mb.

## Hardware acceptance tests

1. Set WLED off, reboot the bridge, and verify the LEDs stay off.
2. Repeat with custom brightness, RGB color and a dynamic effect; verify no startup POST changes the light.
3. Change power and brightness via HA and WLED web UI; check Matter controller state.
4. Change power or brightness via Matter; verify the current effect and secondary colors are unchanged.
5. Disconnect one WLED device; verify reachability changes and other lights remain controllable. Reconnect and verify recovery without replaying stale commands.
6. Add/remove a WLED device, reboot and verify unaffected endpoints retain identity and physical state.
7. Verify Matter pairing and Wi-Fi persist after an application-only OTA with compatible partitions.

Native Apple Adaptive Lighting and hot-plug Matter endpoints are outside this patch and remain unverified. Polling cadence depends on device count and network response times.
