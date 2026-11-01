# Smart IoT Electrical Fault Detection System — V6.0 Reliability Edition

Target: ESP32 DevKit V1 (30-pin), isolated 12 V DC demonstration only.

## V6.0 changes from the V5.3 source
- Unique setup Wi-Fi SSID per board: `SmartFault-XXXX`, where `XXXX` is derived from the board's MAC address. This helps distinguish multiple ESP32 devices nearby.
- Setup portal page text and status endpoint use the runtime SSID.
- Wi-Fi scan response includes channel number as well as signal strength and security indication.
- Dashboard API reports sensor sample age and the UI can show `DATA STALE` if samples stop updating.
- Improved recovery for a saved router that connects after the initial startup connection timeout.
- Existing local dashboard, admin settings, fault indicators, software simulation, LCD, event history, CSV export and Wi-Fi settings remain in the source.

## First boot / Wi-Fi setup
1. Power the ESP32 using the project's regulated low-voltage supply.
2. Open the phone/laptop Wi-Fi list and select the unique network `SmartFault-XXXX` shown for that board. `XXXX` is board-specific; it will not necessarily be exactly `Setup`.
3. Setup password is configured by `AP_PASSWORD` in the sketch; its current default is `DCmonitor123`. Change it before a public demonstration.
4. Open `http://192.168.4.1`.
5. Scan nearby networks, select the correct 2.4 GHz SSID, enter its password and choose **Save and connect**. Hidden SSIDs can be entered manually.
6. After successful router connection, the setup AP is disabled. Connect the phone/laptop to the same router and use the IP shown on the LCD or Serial Monitor (115200 baud). You may also try `http://smartfault-XXXX.local` if mDNS is supported by the LAN.
7. If the router cannot be reached, the setup AP should be restored so the network can be configured again.

## Network limitations and security
- ESP32 supports 2.4 GHz Wi-Fi; a 5 GHz-only network will not connect.
- Router client isolation can prevent the phone/laptop from reaching the ESP32 dashboard.
- Local HTTP is not encrypted. Do not port-forward this device or expose it to the public internet.
- Credentials are stored in ESP32 Preferences/NVS; this application does not encrypt them itself.
- The setup AP currently uses the same configured password on all boards. Change it before demonstration; a per-device password system would be a further security improvement.
- Browser captive-portal popups are not guaranteed; manually open `http://192.168.4.1`.

## Safety
- Never connect this student demonstration to household 230 V AC mains.
- Do not create real short circuits; use software-only simulation.
- ESP32 GPIO/ADC pins are not 5 V tolerant. Verify module outputs and dividers with a multimeter before wiring.
- Relay output remains OFF by default and is not certified automatic protection.
- Sensor plausibility indicators are not proof of sensor health; calibrate and test with a multimeter.

## Validation status
- Source-based edits completed.
- JavaScript syntax and ZIP integrity should be checked before upload (see release notes).
- Firmware has not been compiled against the exact ESP32 Arduino core and installed libraries, and has not been tested on physical hardware. Treat this as a development build until those checks are done.
