# Smart IoT Electrical Fault Detection System — V5.0 Advanced Diagnostics

## V5 additions
- Sensor plausibility panel (`READING` / `CHECK`) for input voltage, output voltage and current. This is a plausibility warning only, not a sensor self-test.
- Fault-specific safe troubleshooting guidance; software simulations are explicitly labeled and never create physical faults.
- Gauge warning colors now use the saved voltage/current thresholds from the admin settings API.
- ESP32 uptime now comes from `millis()` reported by the firmware, rather than browser session time.
- Existing RGB cockpit, six-state fault matrix, trend chart, event list/CSV, admin settings, simulation mode, responsive layout and team page retained.

## Setup
1. Install Arduino IDE and the ESP32 board package.
2. Install a compatible `LiquidCrystal_I2C` library.
3. Open `Smart_IoT_Fault_System_V5.ino`, select your ESP32 board and COM port, then compile before connecting the sensors.
4. Join Wi-Fi `SmartFault-Setup` using `DCmonitor123`.
5. Open `http://192.168.4.1` on the connected phone/computer.
6. Admin user: `admin`; password: `Suraj.bh.com`.

## Validation status
The archive and embedded JavaScript syntax are checked. This environment does not have the ESP32 Arduino toolchain, so the firmware has not been compiled against your exact board package/libraries and has not been tested on physical hardware. Do not treat this as a guarantee of zero bugs. Compile first and resolve any board/library-specific messages before wiring sensors.

## Safety / limitations
- Isolated 12V DC educational prototype only. Never connect this student project to household 230V AC mains.
- Do not create a real short circuit; use software simulation.
- The system provides indicative diagnostics, not certified protection.
- One current sensor measures total current only; it cannot locate the exact fault point or individual parallel branch.
- Power factor is not measured. Energy is an estimate based on sampled power.
- Sensor health flags only catch a few implausible readings; a `READING` status does not prove the sensor is good.
- Event history remains RAM-only and clears on ESP32 restart; CSV exports only the retained event list.
- Admin authentication uses HTTP Basic Auth over local HTTP and is suitable only for a supervised local demonstration. Do not expose it to the internet.
- Verify sensor outputs with a multimeter. ESP32 ADC pins are not 5V tolerant. Adjust and measure the buck converter output before connecting modules.
- Relay output remains OFF by default and is not certified automatic protection.
