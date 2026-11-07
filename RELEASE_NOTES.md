# V6.0 Release Notes

This is a development build based on the provided V5.3 source.

### Implemented in this source revision
- Per-board setup SSID (`SmartFault-XXXX`) to distinguish multiple devices.
- Runtime SSID displayed by the setup portal and Wi-Fi status APIs.
- Channel number added to scanned network metadata.
- Sample-age telemetry and a dashboard stale-data label.
- Late router-connect recovery handling.

### Not yet verified
- Arduino/ESP32 C++ compilation.
- Board-specific behavior across ESP32 Arduino core versions.
- Physical Wi-Fi roaming/recovery, LCD, sensors, buzzer and relay behavior.
- End-to-end admin and Wi-Fi provisioning behavior on real phones.

Do not describe this build as bug-free until compilation and hardware tests pass.
