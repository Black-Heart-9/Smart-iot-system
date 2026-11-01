/*
  Smart IoT Electrical Fault Detection System V5.0 Advanced Diagnostics Edition
  Target: ESP32 DevKit V1 (30-pin), isolated 12V DC demonstration only.

  IMPORTANT SAFETY:
  - Never connect this project to 230V AC mains.
  - Do not create a physical short circuit. Use the software test mode.
  - Verify every module's labels and output voltage before connecting ESP32 ADC pins.
  - ESP32 pins are NOT 5V tolerant.
*/
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>

// ---------- Pin map (match the finalized plan) ----------
static const int PIN_CURRENT = 32;   // ACS712 OUT through a divider
static const int PIN_VIN     = 34;   // input voltage sensor S through divider
static const int PIN_VOUT    = 35;   // output voltage sensor S through divider
static const int PIN_GREEN   = 16;
static const int PIN_YELLOW  = 17;
static const int PIN_RED     = 23;
static const int PIN_BLUE    = 27;
static const int PIN_WHITE   = 14;
static const int PIN_BUZZER  = 25;   // transistor driver input, not buzzer directly
static const int PIN_RELAY   = 26;   // relay module IN; OFF by default
// Physical rocker switches are in the 12V power path, not GPIO inputs.

// ---------- Wi-Fi setup ----------
// The ESP32 starts its own access point so no router is required.
const char *AP_SSID = "SmartFault-Setup";
const char *AP_PASSWORD = "DCmonitor123"; // Change before public demonstration.

// Change this admin password before using the admin page.
const char *ADMIN_USER = "admin";
const char *ADMIN_PASSWORD = "Suraj.bh.com";

WebServer server(80);
Preferences prefs;
LiquidCrystal_I2C lcd(0x27, 20, 4); // If blank, check backpack address (often 0x3F).

// ---------- Calibration ----------
// Each analog output is divided by 2 using 10k series + 10k to GND.
// Adjust voltage calibration after comparing with a multimeter.
float voltageCalibration = 2.00f; // ADC-side voltage x divider ratio, then module ratio below
float voltageModuleRatio = 5.00f; // Typical 0-25V board ratio; VERIFY your module
float currentZeroVolts = 2.50f;  // ACS712 zero-current output at 5V supply; calibrate
float currentSensitivity = 0.100f; // ACS712 20A version: approx 100mV/A
float currentCalibration = 1.00f;

float lowVoltageThreshold = 10.5f;
float highVoltageThreshold = 14.5f;
float overloadThreshold = 0.60f; // Demonstration threshold; adjust for test load
float shortCurrentThreshold = 1.20f;
float openCurrentThreshold = 0.08f;
float openOutputVoltageThreshold = 1.0f;

// ---------- Live data ----------
float inputVoltage = 0, outputVoltage = 0, currentAmps = 0, powerWatts = 0, energyWh = 0;
String faultStatus = "Starting";
String faultSide = "None";
String lastEvent = "System boot";
unsigned long lastSampleMs = 0, lastEnergyMs = 0, lastLcdMs = 0, lastEventMs = 0;
unsigned long bootMs = 0;
bool buzzerEnabled = true;
bool alarmsEnabled = true;
bool relayEnabled = false; // Relay output remains OFF; no switching control is exposed in this student demo.
bool testMode = false;
String testFault = "Normal";
String history[12];
int historyCount = 0;
int historyIndex = 0;
int lcdPage = 0;
String candidateFault = "Starting";
unsigned long candidateFaultSince = 0;
const unsigned long FAULT_CONFIRM_MS = 1200; // require a condition to persist before changing status
float chartVout[60] = {0};
float chartCurrent[60] = {0};
int chartIndex = 0, chartCount = 0;
unsigned long lastChartMs = 0;

float readPinVolts(int pin) {
  // Average samples to reduce noise. ESP32 ADC is approximate; calibrate in practice.
  uint32_t sum = 0;
  for (int i = 0; i < 12; i++) {
    sum += analogRead(pin);
    delayMicroseconds(250);
  }
  float raw = sum / 12.0f;
  return (raw / 4095.0f) * 3.3f;
}

void addEvent(const String &message) {
  if (message == lastEvent && millis() - lastEventMs < 5000) return;
  lastEvent = message;
  lastEventMs = millis();
  history[historyIndex] = String(millis() / 1000) + "s: " + message;
  historyIndex = (historyIndex + 1) % 12;
  if (historyCount < 12) historyCount++;
}

void setRelayOff() {
  // Most common 5V relay modules are active-low. Keep the relay OFF at boot.
  // If your module is active-high, invert this logic after verifying its datasheet.
  digitalWrite(PIN_RELAY, HIGH);
}

void updateSensors() {
  // Sensor outputs pass through 10k/10k dividers; restore original signal x2.
  float vinAdc = readPinVolts(PIN_VIN) * 2.0f;
  float voutAdc = readPinVolts(PIN_VOUT) * 2.0f;
  inputVoltage = vinAdc * voltageModuleRatio * (voltageCalibration / 2.0f);
  outputVoltage = voutAdc * voltageModuleRatio * (voltageCalibration / 2.0f);

  float currentSignal = readPinVolts(PIN_CURRENT) * 2.0f; // undo 10k/10k divider
  currentAmps = ((currentSignal - currentZeroVolts) / currentSensitivity) * currentCalibration;
  if (currentAmps < 0) currentAmps = 0; // one-direction DC demonstration
  if (currentAmps > 25) currentAmps = 25; // sanity clamp, not a protection function
  powerWatts = outputVoltage * currentAmps;

  unsigned long now = millis();
  if (lastEnergyMs == 0) lastEnergyMs = now;
  float elapsedHours = (now - lastEnergyMs) / 3600000.0f;
  if (elapsedHours > 0 && elapsedHours < 0.1f) energyWh += powerWatts * elapsedHours;
  lastEnergyMs = now;
}

void diagnoseRaw() {
  if (testMode) {
    faultStatus = testFault;
    faultSide = (testFault == "Normal") ? "None" : "Software simulation";
    return;
  }
  faultStatus = "Normal";
  faultSide = "None";

  if (inputVoltage > highVoltageThreshold) {
    faultStatus = "High Voltage"; faultSide = "Input / supply";
  } else if (inputVoltage > 0.5f && inputVoltage < lowVoltageThreshold) {
    faultStatus = "Low Voltage"; faultSide = "Input / supply";
  } else if (currentAmps >= shortCurrentThreshold && outputVoltage < inputVoltage * 0.55f) {
    faultStatus = "Short Circuit Suspected"; faultSide = "Output / load";
  } else if (currentAmps >= overloadThreshold) {
    faultStatus = "Overload"; faultSide = "Output / load";
  } else if (inputVoltage > 5.0f && currentAmps < openCurrentThreshold && outputVoltage < openOutputVoltageThreshold) {
    faultStatus = "Open Circuit Suspected"; faultSide = "Output / load";
  }
  // A single total-current sensor cannot identify the exact parallel branch.
}


void diagnose() {
  String previous = faultStatus;
  diagnoseRaw();
  String detected = faultStatus;
  String detectedSide = faultSide;
  // Software simulation is immediate; physical readings use a short confirmation period.
  if (testMode) { candidateFault = detected; candidateFaultSince = millis(); return; }
  if (detected == previous) { candidateFault = detected; candidateFaultSince = millis(); return; }
  if (detected != candidateFault) { candidateFault = detected; candidateFaultSince = millis(); }
  if (millis() - candidateFaultSince >= FAULT_CONFIRM_MS) {
    faultStatus = candidateFault;
    faultSide = detectedSide;
  } else {
    faultStatus = previous;
  }
}

void updateIndicators() {
  bool normal = faultStatus == "Normal";
  bool critical = faultStatus.indexOf("Short Circuit") >= 0 || faultStatus == "High Voltage";
  bool warning = !normal && !critical && faultStatus != "Starting";
  digitalWrite(PIN_GREEN, normal ? HIGH : LOW);
  digitalWrite(PIN_YELLOW, warning ? HIGH : LOW);
  digitalWrite(PIN_RED, critical ? HIGH : LOW);
  digitalWrite(PIN_BLUE, WiFi.softAPIP().toString() == "0.0.0.0" ? LOW : HIGH);
  digitalWrite(PIN_WHITE, testMode ? HIGH : LOW);
  // Use a transistor driver appropriate for the buzzer voltage/current.
  digitalWrite(PIN_BUZZER, (alarmsEnabled && buzzerEnabled && (warning || critical)) ? HIGH : LOW);
  // Keep relay OFF by default; this code does not use it for automatic isolation.
  if (!relayEnabled) setRelayOff();
}

String esc(const String &s) {
  String o = s; o.replace("&", "&amp;"); o.replace("<", "&lt;"); o.replace(">", "&gt;");
  o.replace("\"", "&quot;"); o.replace("'", "&#39;"); return o;
}

String dashboardHtml() {
  String h = R"HTML(<!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1"><title>Smart IoT Fault System · RGB Edition</title><style>
  :root{color-scheme:dark;--bg:#070b17;--panel:#10182a;--line:#243451;--cyan:#38e8ff;--pink:#ff3bd4;--lime:#5cffb1;--text:#eef5ff;--muted:#94a8c9}*{box-sizing:border-box}body{margin:0;background:radial-gradient(ellipse at 10% 0%,#182448 0,transparent 40%),radial-gradient(ellipse at 90% 10%,#29133d 0,transparent 32%),var(--bg);color:var(--text);font:15px Inter,system-ui,Arial}header{padding:22px clamp(16px,4vw,42px);background:#0a1020d9;border-bottom:1px solid #2b4267;box-shadow:0 8px 40px #0005}h1{font-size:clamp(21px,4vw,32px);letter-spacing:-.7px;margin:0 0 6px;background:linear-gradient(90deg,#fff,#60e9ff,#ff69df);background-clip:text;color:transparent}.muted{color:var(--muted);font-size:13px}.wrap{max-width:1250px;margin:auto;padding:clamp(12px,2vw,24px)}.nav{display:flex;gap:9px;flex-wrap:wrap;margin:14px 0 20px}.nav button,button{background:#141f36;color:#eaf4ff;border:1px solid #344a70;border-radius:12px;padding:11px 15px;cursor:pointer;transition:.2s;box-shadow:0 0 0 transparent}.nav button:hover,button:hover{border-color:var(--cyan);box-shadow:0 0 18px #38e8ff20;transform:translateY(-1px)}.nav button.active{background:linear-gradient(110deg,#154d68,#53265f);border-color:#53dff5}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(155px,1fr));gap:13px}.card{background:linear-gradient(145deg,#121d32ed,#0d1425ed);border:1px solid #283a5a;border-radius:18px;padding:17px;box-shadow:0 12px 30px #0002;min-width:0}.label{font-size:12px;color:#9fb4d7;text-transform:uppercase;letter-spacing:1.3px}.value{font-size:clamp(24px,4vw,32px);font-weight:750;margin-top:8px;overflow-wrap:anywhere;font-variant-numeric:tabular-nums}.status{font-size:clamp(18px,3vw,24px);font-weight:750}.pill{display:inline-block;padding:6px 10px;border-radius:99px;background:#152943;color:#9eefff;border:1px solid #315477;font-size:12px}.section{display:none}.section.active{display:block}.row{display:flex;gap:10px;flex-wrap:wrap;align-items:center;margin:8px 0}input,select{background:#091122;color:#eef;border:1px solid #405171;padding:10px;border-radius:9px;max-width:100%}pre{white-space:pre-wrap;overflow-wrap:anywhere;color:#c9d8f3}.warn{color:#ffd166}.danger{color:#ff607c}.hero{display:flex;justify-content:space-between;gap:16px;align-items:center;flex-wrap:wrap;margin:4px 0 16px}.live{display:inline-flex;align-items:center;gap:8px;color:#7cffc5;font-size:12px;letter-spacing:1px}.dot{width:8px;height:8px;border-radius:50%;background:#5cffb1;box-shadow:0 0 14px #5cffb1;animation:pulse 1.6s infinite}@keyframes pulse{50%{opacity:.4;transform:scale(.8)}}.gauges{display:grid;grid-template-columns:repeat(auto-fit,minmax(190px,1fr));gap:15px;margin:14px 0}.gauge-card{text-align:center;padding:20px 12px}.gauge{--pct:0%;--gcolor:#5cffb1;width:150px;height:150px;max-width:100%;aspect-ratio:1;margin:15px auto 10px;border-radius:50%;position:relative;display:grid;place-items:center;background:conic-gradient(from 220deg,var(--gcolor) 0 var(--pct),#25334d var(--pct) 280deg,transparent 280deg 360deg);filter:drop-shadow(0 0 12px color-mix(in srgb,var(--gcolor),transparent 70%))}.gauge:before{content:'';position:absolute;inset:10px;border-radius:50%;background:radial-gradient(circle at 35% 25%,#172642,#090f1e 75%);border:1px solid #304361}.gauge-inner{z-index:1}.gauge-num{font-size:27px;font-weight:800;font-variant-numeric:tabular-nums}.gauge-unit{font-size:12px;color:#9db2d2}.gauge-caption{font-size:12px;color:#9db2d2;letter-spacing:1.3px;text-transform:uppercase}.fault-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(155px,1fr));gap:10px}.fault-tile{border:1px solid #293b58;background:#0d1526;border-radius:13px;padding:13px;display:flex;gap:10px;align-items:center;min-height:68px}.fault-lamp{width:11px;height:11px;border-radius:50%;background:#3e536d;flex-shrink:0}.fault-tile.active{border-color:#ff4f79;background:#301524;box-shadow:0 0 20px #ff286d2b;animation:faultBlink .85s infinite}.fault-tile.active .fault-lamp{background:#ff476f;box-shadow:0 0 13px #ff476f;animation:pulse .7s infinite}.fault-tile.ok{border-color:#235b50}.fault-tile.ok .fault-lamp{background:#55ffb2;box-shadow:0 0 10px #55ffb266}@keyframes faultBlink{50%{border-color:#ff9eaf;box-shadow:0 0 24px #ff286d55}}.toast-area{position:fixed;right:14px;top:14px;z-index:999;display:grid;gap:8px;width:min(360px,calc(100vw - 28px))}.toast{padding:14px 16px;border-radius:14px;background:#171e32;border:1px solid #ff527b;box-shadow:0 12px 40px #0009;animation:slidein .25s ease}.toast strong{color:#ff8ba6;display:block;margin-bottom:3px}@keyframes slidein{from{transform:translateX(20px);opacity:0}to{transform:none;opacity:1}}.section h2,.section h3{letter-spacing:-.3px}.team-grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:12px}.person{border:1px solid #2b3c5b;background:#0b1426;border-radius:14px;padding:15px}.person strong{display:block;color:#e9f8ff;margin-bottom:7px}.person .role{font-size:11px;letter-spacing:1px;color:#5fe6ff;text-transform:uppercase;margin-bottom:8px}canvas{max-width:100%}footer{color:#8499bc;font-size:12px;padding:22px 0}@media(max-width:540px){.gauge{width:132px;height:132px}.card{padding:14px}.nav button{flex:1}.fault-grid{grid-template-columns:repeat(2,minmax(0,1fr))}.fault-tile{font-size:12px;padding:10px}}
  
  /* V5.0 premium cockpit refinement */
  body{background:radial-gradient(ellipse at 12% -10%,#123c55 0,transparent 38%),radial-gradient(ellipse at 92% 4%,#45134a 0,transparent 32%),linear-gradient(135deg,#050812,#080c19 55%,#100819);background-attachment:fixed}
  header{position:relative;overflow:hidden;background:linear-gradient(110deg,#080d1deF,#10112beF);border-bottom:1px solid #3a3c72}
  header:after{content:"";position:absolute;left:0;right:0;bottom:0;height:2px;background:linear-gradient(90deg,#00e5ff,#a855f7,#ff35c8,#00e5ff);background-size:240% 100%;animation:rgbFlow 8s linear infinite}
  @keyframes rgbFlow{to{background-position:240% 0}}
  .wrap{max-width:1440px}.card{background:linear-gradient(145deg,rgba(17,28,50,.94),rgba(9,14,29,.96));border-color:#283b60;box-shadow:0 18px 48px #0004,inset 0 1px #ffffff08;backdrop-filter:blur(12px)}
  .gauges{grid-template-columns:repeat(3,minmax(0,1fr));gap:18px}.gauge-card{position:relative;overflow:hidden;border-color:#31476c}.gauge-card:before{content:"";position:absolute;inset:0 20% auto;height:1px;background:linear-gradient(90deg,transparent,#3eeaff,transparent)}
  .gauge{width:min(190px,100%);height:auto;aspect-ratio:1;transition:filter .25s,background .25s}.gauge-num{font-size:clamp(23px,3vw,34px);text-shadow:0 0 20px color-mix(in srgb,var(--gcolor),transparent 60%)}
  .system-strip{max-width:1440px;margin:16px auto 0;padding:0 clamp(12px,2vw,24px);display:grid;grid-template-columns:repeat(4,minmax(0,1fr));gap:10px}.strip-item{display:flex;align-items:center;gap:9px;flex-wrap:wrap;padding:11px 13px;border:1px solid #283756;border-radius:12px;background:#0b1224d9;color:#8fa6ca;font-size:10px;letter-spacing:1px}.strip-item b{margin-left:auto;color:#dffaff;font-size:11px;letter-spacing:.4px}.strip-dot{width:7px;height:7px;border-radius:50%;background:#5cffb1;box-shadow:0 0 12px #5cffb1}
  .nav{position:sticky;top:0;z-index:30;padding:8px;background:#070b17d9;backdrop-filter:blur(14px);border:1px solid #253451;border-radius:15px}.nav button{flex:1;min-width:110px}.nav button.active{box-shadow:0 0 22px #38e8ff20,inset 0 0 14px #38e8ff12}
  .fault-tile{transition:transform .18s,border-color .18s;min-height:78px}.fault-tile:hover{transform:translateY(-2px);border-color:#4b6797}.fault-tile.active{animation:faultBlink .9s infinite}.status{overflow-wrap:anywhere}.section h2{font-size:clamp(22px,3vw,30px)}
  button:focus-visible,input:focus-visible,select:focus-visible{outline:2px solid #45eaff;outline-offset:2px}.nav button:focus-visible{outline-offset:3px}
  @media(max-width:720px){.gauges{grid-template-columns:1fr;}.gauge{width:165px}.system-strip{grid-template-columns:repeat(2,minmax(0,1fr))}.strip-item{font-size:9px;padding:10px}.nav{position:static}}
  @media(prefers-reduced-motion:reduce){*,*:before,*:after{animation-duration:.01ms!important;animation-iteration-count:1!important;scroll-behavior:auto!important;transition:none!important}}
  </style></head><body><header><h1>Smart IoT Electrical Fault Detection System</h1><div class="muted">VERSION 4.0 · RGB COCKPIT · 12V DC DEMONSTRATION</div></header><main class="wrap"><div class="nav"><button class="active" onclick="showTab('monitor',this)">Monitoring</button><button onclick="showTab('about',this)">About Project</button><button onclick="showTab('admin',this)">Admin Panel</button></div>
  <div class="system-strip"><div class="strip-item"><span class="strip-dot"></span><span>ESP32 LOCAL LINK</span><b id="linkState">CONNECTING</b></div><div class="strip-item"><span>ESP32 UPTIME</span><b id="uptime">--</b></div><div class="strip-item"><span>LAST UPDATE</span><b id="lastUpdate">--</b></div><div class="strip-item"><span>MODE</span><b id="topMode">LIVE</b></div></div><div id="toastArea" class="toast-area" aria-live="assertive"></div><section id="monitor" class="section active"><div class="hero"><div><div class="live"><span class="dot"></span> SYSTEM LIVE</div><h2 style="margin:8px 0 4px">Electrical telemetry</h2><div class="muted">Real-time DC supply and load health · RGB diagnostics</div></div><div class="row"><button onclick="if('Notification' in window)Notification.requestPermission().then(x=>showToast('Browser alerts','Permission: '+x));else showToast('Browser alerts','Notifications are not supported here.')">Enable notifications</button><span id="mode" class="pill">Live sensor mode</span></div></div>
  <div class="gauges"><div class="card gauge-card"><div class="gauge-caption">Input voltage</div><div class="gauge" id="gVin"><div class="gauge-inner"><div class="gauge-num"><span id="vin">--</span></div><div class="gauge-unit">VOLTS</div></div></div><div class="muted">Supply rail</div></div><div class="card gauge-card"><div class="gauge-caption">Output voltage</div><div class="gauge" id="gVout"><div class="gauge-inner"><div class="gauge-num"><span id="vout">--</span></div><div class="gauge-unit">VOLTS</div></div></div><div class="muted">Load rail</div></div><div class="card gauge-card"><div class="gauge-caption">Load current</div><div class="gauge" id="gAmps"><div class="gauge-inner"><div class="gauge-num"><span id="amps">--</span></div><div class="gauge-unit">AMPERES</div></div></div><div class="muted">Total measured current</div></div></div>
  <div class="grid"><div class="card"><div class="label">Estimated power</div><div class="value"><span id="watts">--</span> <span style="font-size:14px;color:#9db2d2">W</span></div></div><div class="card"><div class="label">Estimated energy</div><div class="value"><span id="wh">--</span> <span style="font-size:14px;color:#9db2d2">Wh</span></div></div><div class="card"><div class="label">Current system status</div><div id="status" class="status" style="margin-top:9px">Connecting…</div><p class="muted">Indicated section: <b id="side">--</b></p></div></div>
  <div class="card" style="margin-top:15px"><h3>Sensor health & diagnostics</h3><p id="sensorHealth" class="muted">Waiting for sensor data…</p><p id="diagnosis">Waiting for diagnostic status…</p><p class="muted">Sensor status is a plausibility check, not proof that a sensor is electrically healthy.</p></div><div class="card" style="margin-top:15px"><div class="hero"><div><h3 style="margin:0 0 5px">Fault matrix</h3><div class="muted">A fault tile blinks when the system reports that condition.</div></div><span id="faultSummary" class="pill">No active fault</span></div><div class="fault-grid"><div class="fault-tile" id="fault-short"><span class="fault-lamp"></span><span><b>Short Circuit Suspected</b><br><span class="muted">Output/load</span></span></div><div class="fault-tile" id="fault-open"><span class="fault-lamp"></span><span><b>Open Circuit Suspected</b><br><span class="muted">Output/load</span></span></div><div class="fault-tile" id="fault-over"><span class="fault-lamp"></span><span><b>Overload</b><br><span class="muted">Current high</span></span></div><div class="fault-tile" id="fault-low"><span class="fault-lamp"></span><span><b>Low Voltage</b><br><span class="muted">Supply low</span></span></div><div class="fault-tile" id="fault-high"><span class="fault-lamp"></span><span><b>High Voltage</b><br><span class="muted">Supply high</span></span></div><div class="fault-tile ok" id="fault-normal"><span class="fault-lamp"></span><span><b>Normal Operation</b><br><span class="muted">No detected fault</span></span></div></div></div>
  <div class="card" style="margin-top:15px"><h3>Live trend · last 60 samples</h3><canvas id="trend" width="900" height="220" style="width:100%;height:auto;background:#091122;border-radius:12px"></canvas><div class="muted">Output voltage and current trend. Values are for demonstration, not a certified protection instrument.</div></div><div class="card" style="margin-top:15px"><h3>Recent system events</h3><pre id="history">Loading…</pre></div></section>
    <section id="about" class="section"><div class="card"><div class="live"><span class="dot"></span> TEAM & PROJECT</div><h2>About the project</h2><p>This project demonstrates local IoT monitoring of an isolated 12V DC test circuit using ESP32, voltage sensors and a current sensor. It reports measurements and indicates suspected abnormal conditions on the LCD and a live dashboard.</p><h3>Team</h3><p class="muted">Team project · Second Year Electrician Trade</p><h3>Contributions for this project</h3><div class="team-grid"><div class="person"><div class="role">Components & Hardware</div><strong>Soupitk Santra</strong>Led component purchasing and sourcing, helped arrange required parts, and worked on hardware connections and assembly.</div><div class="person"><div class="role">Programming & Integration</div><strong>Suraj Datta</strong>Designed and implemented the project coding and programming, helped prepare the item-wise component list, and assisted with circuit connections and integration.</div><div class="person"><div class="role">Component Research</div><strong>Prabhas Bhunia</strong>Helped identify required components, compare options, and coordinate component sourcing and project preparation.</div><div class="person"><div class="role">Assembly Support</div><strong>Devkrishna Kanta Samai</strong>Supported component planning, hardware preparation, and assembly tasks during the project build.</div><div class="person"><div class="role">Build & Testing Support</div><strong>Surajit Samai</strong>Assisted with component coordination, wiring support, checking connections, and preparing the demonstration setup.</div></div><p class="muted" style="margin-top:14px">Contribution descriptions summarize the team's stated work and should be adjusted if the actual task split changes.</p><h3>System limitations</h3><p>Fault identification is indicative, not certified protection. Sensor-health flags are plausibility checks and cannot prove sensor integrity. One current sensor measures total current and cannot identify an individual parallel branch. Power factor is not measured in this DC project; energy is estimated.</p><p class="pill">Approximate project budget: ₹4,500</p></div></section>
    <section id="admin" class="section"><div class="card"><h2>Admin Panel</h2><p class="muted">Browser will ask for the admin username and password.</p><button onclick="loadAdmin()">Unlock / Load Settings</button><div id="adminBox" style="margin-top:14px"></div></div></section>
  <footer>V5.0 RGB COCKPIT · Use only with an isolated 12V DC supply. Never connect to household AC mains. Do not physically short the supply. Fault indications are advisory, not certified protection.</footer></main><script>
  function showTab(id,b){document.querySelectorAll('.section').forEach(x=>x.classList.remove('active'));document.getElementById(id).classList.add('active');document.querySelectorAll('.nav button').forEach(x=>x.classList.remove('active'));b.classList.add('active')}
  let previousFault='',lastNotifyAt=0,lastSuccessAt=0;function diagnosisText(status,side,simulated){if(simulated)return 'Software simulation active: '+status+'. No physical fault has been created.';const advice={'Normal':'No fault is currently indicated. Verify readings against a multimeter.','Low Voltage':'Check the isolated DC adapter, wiring, fuse and terminal tightness with power safely removed.','High Voltage':'Switch off the isolated low-voltage supply and verify adapter output with a multimeter.','Overload':'Remove power before checking the load rating, wiring and sensor calibration.','Short Circuit Suspected':'Do not create a real short. Remove power and inspect the load/wiring for accidental shorts.','Open Circuit Suspected':'Remove power and inspect disconnected terminals, fuse continuity and load wiring.'};return (advice[status]||'Check sensor readings and calibration.')+' Monitored section: '+(side||'not identified')+'.';}function setGauge(id,val,max,low,high){let el=document.getElementById(id);if(!el)return;let n=Number(val);let pct=Math.max(0,Math.min(1,(Number.isFinite(n)?n:0)/max));let col=(!Number.isFinite(n)||n<0)?'#94a8c9':(n<low||n>high)?'#ff476f':(n<low*1.1||n>high*.9)?'#ffe16b':'#5cffb1';el.style.setProperty('--pct',(pct*280)+'deg');el.style.setProperty('--gcolor',col)}function showToast(title,msg){let area=document.getElementById('toastArea');let t=document.createElement('div');t.className='toast';t.innerHTML='<strong>⚠ '+title+'</strong><span>'+msg.replace(/[<>]/g,'')+'</span>';area.prepend(t);setTimeout(()=>t.remove(),6500)}function updateFaultTiles(status){let map={'fault-short':status.includes('Short'),'fault-open':status.includes('Open'),'fault-over':status==='Overload','fault-low':status==='Low Voltage','fault-high':status==='High Voltage','fault-normal':status==='Normal'};Object.keys(map).forEach(id=>{let el=document.getElementById(id);el.classList.toggle('active',!!map[id]);el.classList.toggle('ok',id==='fault-normal'&&!!map[id])});document.getElementById('faultSummary').textContent=status==='Normal'?'No active fault':'ACTIVE: '+status}async function update(){try{let r=await fetch('/api',{cache:'no-store'});if(!r.ok)throw Error('HTTP '+r.status);let d=await r.json();lastSuccessAt=Date.now();document.getElementById('linkState').style.color='';['vin','vout','amps','watts','wh'].forEach(k=>{const el=document.getElementById(k);const n=Number(d[k]);el.textContent=Number.isFinite(n)?n.toFixed(2):'--';});setGauge('gVin',d.vin,16,Number(d.thresholds?.lowV??10.5),Number(d.thresholds?.highV??14.5));setGauge('gVout',d.vout,16,Number(d.thresholds?.lowV??10.5),Number(d.thresholds?.highV??14.5));setGauge('gAmps',d.amps,2,0,Number(d.thresholds?.overA??0.6));let s=document.getElementById('status');s.textContent=d.status+' — '+d.side;s.className='status '+(d.status==='Normal'?'':(d.status.includes('Short')||d.status.includes('High')?'danger':'warn'));document.getElementById('mode').textContent=d.testMode?'SOFTWARE TEST MODE: '+d.testFault:'Live sensor mode';document.getElementById('topMode').textContent=d.testMode?'DEMO':'LIVE';document.getElementById('side').textContent=d.side||'None';document.getElementById('linkState').textContent='ONLINE';document.getElementById('uptime').textContent=Math.floor(Number(d.uptimeMs||0)/1000)+'s';document.getElementById('lastUpdate').textContent=new Date().toLocaleTimeString();
let health=d.sensorHealth||{};let healthEl=document.getElementById('sensorHealth');if(healthEl)healthEl.textContent='Input: '+(health.inputVoltage||'UNKNOWN')+' · Output: '+(health.outputVoltage||'UNKNOWN')+' · Current: '+(health.current||'UNKNOWN');let diag=document.getElementById('diagnosis');if(diag)diag.textContent=diagnosisText(d.status,d.side,d.testMode);document.getElementById('history').textContent=(d.history||[]).join('\n')||'No events yet';updateFaultTiles(d.status);if(previousFault&&d.status!==previousFault&&d.status!=='Normal'){showToast('FAULT ALERT',d.status+' · '+d.side);if('Notification'in window&&Notification.permission==='granted')new Notification('Smart Fault System: '+d.status,{body:'Indicated section: '+d.side});}if(d.status==='Normal'&&previousFault&&previousFault!=='Normal')showToast('System recovered','Monitoring reports normal operation.');previousFault=d.status;drawTrend(d.trendVout,d.trendCurrent);}catch(e){document.getElementById('status').textContent='Connection lost — readings unavailable';document.getElementById('linkState').textContent='OFFLINE';document.getElementById('linkState').style.color='#ff607c';}}document.addEventListener('click',e=>{if(e.target&&e.target.textContent&&e.target.textContent.includes('Enable alerts')&&'Notification'in window)Notification.requestPermission();});
  function drawTrend(v,c){const canvas=document.getElementById('trend');if(!canvas)return;const ctx=canvas.getContext('2d');const w=canvas.width,h=canvas.height;ctx.clearRect(0,0,w,h);ctx.strokeStyle='#2b3b59';ctx.lineWidth=1;for(let i=1;i<5;i++){ctx.beginPath();ctx.moveTo(0,h*i/5);ctx.lineTo(w,h*i/5);ctx.stroke()}function line(a,max){if(!a||a.length<2)return;ctx.beginPath();ctx.strokeStyle='#72b7ff';a.forEach((x,i)=>{let px=i*w/(a.length-1),py=h-12-Math.max(0,Math.min(1,x/(max||1)))*(h-24);if(i===0)ctx.moveTo(px,py);else ctx.lineTo(px,py)});ctx.stroke()}line(v,16);ctx.strokeStyle='#ffc66d';if(c&&c.length>1){ctx.beginPath();c.forEach((x,i)=>{let px=i*w/(c.length-1),py=h-12-Math.max(0,Math.min(1,x/3))*(h-24);if(i===0)ctx.moveTo(px,py);else ctx.lineTo(px,py)});ctx.stroke()}}
  async function loadAdmin(){try{let r=await fetch('/admin/settings');if(!r.ok)throw Error('Authentication failed');let d=await r.json();document.getElementById('adminBox').innerHTML=`<div class="row">Low V <input id="low" type="number" step="0.1" value="${d.low}"> High V <input id="high" type="number" step="0.1" value="${d.high}"></div><div class="row">Overload A <input id="over" type="number" step="0.05" value="${d.over}"> Short suspected A <input id="short" type="number" step="0.1" value="${d.short}"></div><div class="row">Voltage module ratio <input id="ratio" type="number" step="0.1" value="${d.ratio}"> Voltage calibration <input id="vcal" type="number" step="0.01" value="${d.vcal}"></div><div class="row">Current zero V <input id="zero" type="number" step="0.01" value="${d.zero}"> Current sensitivity V/A <input id="sens" type="number" step="0.001" value="${d.sens}"></div><div class="row"><label><input id="alarm" type="checkbox" ${d.alarm?'checked':''}> Alarms enabled</label><label><input id="buzz" type="checkbox" ${d.buzz?'checked':''}> Buzzer enabled</label></div><div class="row"><select id="test"><option>Normal</option><option>Low Voltage</option><option>High Voltage</option><option>Overload</option><option>Open Circuit Suspected</option><option>Short Circuit Suspected</option></select><button onclick="setTest()">Apply software test</button><button onclick="saveSettings()">Save settings</button><button onclick="resetEnergy()">Reset energy</button><button onclick="restartBoard()">Restart ESP32</button><button onclick="downloadEvents()">Export event log CSV</button></div><p class="muted">Use software test mode only; it does not create a physical electrical fault.</p>`;}catch(e){document.getElementById('adminBox').textContent='Could not load settings. Check credentials and try again.';}}
  async function saveSettings(){let p={low:+low.value,high:+high.value,over:+over.value,short:+short.value,ratio:+ratio.value,vcal:+vcal.value,zero:+zero.value,sens:+sens.value,alarm:alarm.checked,buzz:buzz.checked};let r=await fetch('/admin/settings',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(p)});alert(r.ok?'Settings saved':'Save failed');}
  async function setTest(){let r=await fetch('/admin/test?fault='+encodeURIComponent(test.value),{method:'POST'});alert(r.ok?'Test mode updated':'Update failed');}
  async function resetEnergy(){let r=await fetch('/admin/energy-reset',{method:'POST'});alert(r.ok?'Energy counter reset':'Reset failed');}
  async function restartBoard(){if(confirm('Restart ESP32?')){await fetch('/admin/restart',{method:'POST'});}}
  function downloadEvents(){window.location.href='/admin/events.csv';}
  setInterval(update,1000);update();
  </script></body></html>)HTML";
  return h;
}

bool requireAdmin() {
  if (!server.authenticate(ADMIN_USER, ADMIN_PASSWORD)) {
    server.requestAuthentication(BASIC_AUTH, "SmartFault Admin", "Admin login required");
    return false;
  }
  return true;
}

String historyJson() {
  String s = "[";
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + 12) % 12;
    if (i) s += ",";
    s += "\"" + esc(history[idx]) + "\"";
  }
  s += "]";
  return s;
}

void sendApi() {
  String j = "{";
  j += "\"vin\":" + String(inputVoltage, 3) + ",\"vout\":" + String(outputVoltage, 3);
  j += ",\"amps\":" + String(currentAmps, 3) + ",\"watts\":" + String(powerWatts, 3) + ",\"wh\":" + String(energyWh, 4);
  j += ",\"status\":\"" + esc(faultStatus) + "\",\"side\":\"" + esc(faultSide) + "\"";
  j += ",\"testMode\":" + String(testMode ? "true" : "false") + ",\"testFault\":\"" + esc(testFault) + "\"";
  j += ",\"uptimeMs\":" + String(millis());
  j += ",\"thresholds\":{" + String("\"lowV\":") + String(lowVoltageThreshold, 2) + ",\"highV\":" + String(highVoltageThreshold, 2) + ",\"overA\":" + String(overloadThreshold, 2) + ",\"shortA\":" + String(shortCurrentThreshold, 2) + "}";
  bool adcRailVin = inputVoltage < 0.15f || inputVoltage > 30.0f;
  bool adcRailVout = outputVoltage < 0.15f || outputVoltage > 30.0f;
  bool adcRailCurrent = currentAmps >= 24.9f;
  j += ",\"sensorHealth\":{" + String("\"inputVoltage\":\"") + (adcRailVin ? "CHECK" : "READING") + "\",\"outputVoltage\":\"" + (adcRailVout ? "CHECK" : "READING") + "\",\"current\":\"" + (adcRailCurrent ? "CHECK" : "READING") + "\"}";
  j += ",\"history\":" + historyJson();
  j += ",\"trendVout\":[";
  for (int i = 0; i < chartCount; i++) { if (i) j += ","; int idx = (chartIndex - chartCount + i + 60) % 60; j += String(chartVout[idx], 2); }
  j += "],\"trendCurrent\":[";
  for (int i = 0; i < chartCount; i++) { if (i) j += ","; int idx = (chartIndex - chartCount + i + 60) % 60; j += String(chartCurrent[idx], 2); }
  j += "]}";
  server.send(200, "application/json", j);
}

void sendAdminSettings() {
  if (!requireAdmin()) return;
  String j = "{";
  j += "\"low\":" + String(lowVoltageThreshold, 2) + ",\"high\":" + String(highVoltageThreshold, 2);
  j += ",\"over\":" + String(overloadThreshold, 2) + ",\"short\":" + String(shortCurrentThreshold, 2);
  j += ",\"ratio\":" + String(voltageModuleRatio, 2) + ",\"vcal\":" + String(voltageCalibration, 2);
  j += ",\"zero\":" + String(currentZeroVolts, 3) + ",\"sens\":" + String(currentSensitivity, 3);
  j += ",\"alarm\":" + String(alarmsEnabled ? "true" : "false") + ",\"buzz\":" + String(buzzerEnabled ? "true" : "false") + "}";
  server.send(200, "application/json", j);
}

void handleAdminSave() {
  if (!requireAdmin()) return;
  String body = server.arg("plain");
  auto getNum = [&](const char *key, float current) -> float {
    String needle = String("\"") + key + "\":";
    int p = body.indexOf(needle); if (p < 0) return current;
    p += needle.length(); int e = p;
    while (e < (int)body.length() && String("0123456789.-").indexOf(body[e]) >= 0) e++;
    return body.substring(p, e).toFloat();
  };
  lowVoltageThreshold = constrain(getNum("low", lowVoltageThreshold), 0.0f, 12.0f);
  highVoltageThreshold = constrain(getNum("high", highVoltageThreshold), 12.1f, 25.0f);
  overloadThreshold = constrain(getNum("over", overloadThreshold), 0.05f, 2.0f);
  shortCurrentThreshold = constrain(getNum("short", shortCurrentThreshold), overloadThreshold, 3.0f);
  voltageModuleRatio = constrain(getNum("ratio", voltageModuleRatio), 0.1f, 10.0f);
  voltageCalibration = constrain(getNum("vcal", voltageCalibration), 0.1f, 5.0f);
  currentZeroVolts = constrain(getNum("zero", currentZeroVolts), 0.5f, 3.0f);
  currentSensitivity = constrain(getNum("sens", currentSensitivity), 0.02f, 0.2f);
  alarmsEnabled = body.indexOf("\"alarm\":true") >= 0;
  buzzerEnabled = body.indexOf("\"buzz\":true") >= 0;
  prefs.begin("faultsys", false);
  prefs.putFloat("low", lowVoltageThreshold); prefs.putFloat("high", highVoltageThreshold);
  prefs.putFloat("over", overloadThreshold); prefs.putFloat("short", shortCurrentThreshold);
  prefs.putFloat("ratio", voltageModuleRatio); prefs.putFloat("vcal", voltageCalibration);
  prefs.putFloat("zero", currentZeroVolts); prefs.putFloat("sens", currentSensitivity);
  prefs.putBool("alarm", alarmsEnabled); prefs.putBool("buzz", buzzerEnabled); prefs.end();
  addEvent("Admin settings updated");
  server.send(200, "application/json", "{\"ok\":true}");
}

void handleAdminTest() {
  if (!requireAdmin()) return;
  String f = server.arg("fault");
  const String allowed[] = {"Normal", "Low Voltage", "High Voltage", "Overload", "Open Circuit Suspected", "Short Circuit Suspected"};
  bool valid = false;
  for (auto &x : allowed) if (f == x) valid = true;
  if (!valid) { server.send(400, "text/plain", "Invalid fault"); return; }
  testMode = (f != "Normal"); testFault = f;
  addEvent(testMode ? "Software test: " + f : "Software test stopped");
  server.send(200, "application/json", "{\"ok\":true}");
}

void updateLCD() {
  if (millis() - lastLcdMs < 2500) return;
  lastLcdMs = millis();
  lcd.clear();
  if (millis() - bootMs < 10000) {
    lcd.setCursor(0,0); lcd.print("Smart IoT Fault V5.0");
    lcd.setCursor(0,1);
    if (millis() - bootMs < 2000) lcd.print("Welcome to System");
    else if (millis() - bootMs < 5000) { lcd.print("Countdown: "); lcd.print(3 - ((millis() - bootMs - 2000) / 1000)); }
    else if (millis() - bootMs < 8000) lcd.print("Initializing...");
    else lcd.print("WiFi: AP Ready");
    lcd.setCursor(0,2); lcd.print(AP_SSID);
    lcd.setCursor(0,3); lcd.print(WiFi.softAPIP().toString());
    return;
  }
  if (lcdPage == 0) {
    lcd.setCursor(0,0); lcd.print("In:"); lcd.print(inputVoltage,1); lcd.print("V Out:"); lcd.print(outputVoltage,1); lcd.print("V");
    lcd.setCursor(0,1); lcd.print("Current: "); lcd.print(currentAmps,2); lcd.print(" A");
    lcd.setCursor(0,2); lcd.print("Power: "); lcd.print(powerWatts,1); lcd.print(" W");
    lcd.setCursor(0,3); lcd.print("Energy: "); lcd.print(energyWh,2); lcd.print(" Wh");
  } else {
    lcd.setCursor(0,0); lcd.print("FAULT STATUS:");
    lcd.setCursor(0,1); lcd.print(faultStatus.substring(0,20));
    lcd.setCursor(0,2); lcd.print("Side:"); lcd.print(faultSide.substring(0,14));
    lcd.setCursor(0,3); lcd.print(WiFi.softAPIP().toString());
  }
  lcdPage = 1 - lcdPage;
}

void handleAdminEventsCsv() {
  if (!requireAdmin()) return;
  String csv = "uptime,event\r\n";
  for (int i = 0; i < historyCount; i++) {
    int idx = (historyIndex - historyCount + i + 12) % 12;
    String line = history[idx];
    int sep = line.indexOf("s: ");
    String uptime = sep >= 0 ? line.substring(0, sep) : "";
    String event = sep >= 0 ? line.substring(sep + 3) : line;
    event.replace("\"", "\"\"");
    csv += uptime + ",\"" + event + "\"\r\n";
  }
  server.sendHeader("Content-Disposition", "attachment; filename=smart_fault_events.csv");
  server.send(200, "text/csv; charset=utf-8", csv);
}

void setup() {
  bootMs = millis();
  Serial.begin(115200);
  pinMode(PIN_GREEN, OUTPUT); pinMode(PIN_YELLOW, OUTPUT); pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_BLUE, OUTPUT); pinMode(PIN_WHITE, OUTPUT); pinMode(PIN_BUZZER, OUTPUT); pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_GREEN, LOW); digitalWrite(PIN_YELLOW, LOW); digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_BLUE, LOW); digitalWrite(PIN_WHITE, LOW); digitalWrite(PIN_BUZZER, LOW); setRelayOff();
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_CURRENT, ADC_11db);
  analogSetPinAttenuation(PIN_VIN, ADC_11db);
  analogSetPinAttenuation(PIN_VOUT, ADC_11db);
  Wire.begin(21, 22);
  lcd.init(); lcd.backlight(); lcd.clear(); lcd.setCursor(0,0); lcd.print("Smart IoT Fault V5.0"); lcd.setCursor(0,1); lcd.print("Starting...");

  prefs.begin("faultsys", true);
  lowVoltageThreshold = prefs.getFloat("low", lowVoltageThreshold);
  highVoltageThreshold = prefs.getFloat("high", highVoltageThreshold);
  overloadThreshold = prefs.getFloat("over", overloadThreshold);
  shortCurrentThreshold = prefs.getFloat("short", shortCurrentThreshold);
  voltageModuleRatio = prefs.getFloat("ratio", voltageModuleRatio);
  voltageCalibration = prefs.getFloat("vcal", voltageCalibration);
  currentZeroVolts = prefs.getFloat("zero", currentZeroVolts);
  currentSensitivity = prefs.getFloat("sens", currentSensitivity);
  alarmsEnabled = prefs.getBool("alarm", alarmsEnabled); buzzerEnabled = prefs.getBool("buzz", buzzerEnabled);
  prefs.end();

  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID, AP_PASSWORD);
  Serial.print("Dashboard IP: "); Serial.println(WiFi.softAPIP());
  addEvent("Access point started at " + WiFi.softAPIP().toString());

  server.on("/", HTTP_GET, [](){ server.send(200, "text/html; charset=utf-8", dashboardHtml()); });
  server.on("/api", HTTP_GET, sendApi);
  server.on("/admin/settings", HTTP_GET, sendAdminSettings);
  server.on("/admin/settings", HTTP_POST, handleAdminSave);
  server.on("/admin/test", HTTP_POST, handleAdminTest);
  server.on("/admin/energy-reset", HTTP_POST, [](){ if(!requireAdmin()) return; energyWh=0; addEvent("Energy counter reset"); server.send(200,"application/json","{\"ok\":true}"); });
  server.on("/admin/restart", HTTP_POST, [](){ if(!requireAdmin()) return; server.send(200,"text/plain","Restarting"); delay(250); ESP.restart(); });
  server.on("/admin/events.csv", HTTP_GET, handleAdminEventsCsv);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();
}

void loop() {
  server.handleClient();
  if (millis() - lastSampleMs >= 500) {
    lastSampleMs = millis();
    updateSensors();
    if (millis() - lastChartMs >= 1000) {
      lastChartMs = millis();
      chartVout[chartIndex] = outputVoltage;
      chartCurrent[chartIndex] = currentAmps;
      chartIndex = (chartIndex + 1) % 60;
      if (chartCount < 60) chartCount++;
    }
    String previous = faultStatus;
    diagnose();
    if (faultStatus != previous) addEvent("Status: " + faultStatus);
    updateIndicators();
  }
  updateLCD();
  delay(2);
}
