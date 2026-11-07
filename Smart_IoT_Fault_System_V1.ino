/*
  Smart IoT Electrical Fault Detection System V 1.0 Reliability Edition
  
*/
#include <WiFi.h>
#include <WebServer.h>
#include <Wire.h>
#include <LiquidCrystal_I2C.h>
#include <Preferences.h>
#include <ESPmDNS.h>

// ---------- Pin map (V6.1: matches the available red/green/yellow LEDs) ----------
static const int PIN_CURRENT = 32;   
static const int PIN_VIN     = 34;   
static const int PIN_VOUT    = 35;   
static const int PIN_GREEN   = 16;
static const int PIN_YELLOW  = 17;
static const int PIN_RED     = 23;
static const int PIN_BUZZER  = 25;   
static const int PIN_RELAY   = 26;   

// ---------- Wi-Fi setup ----------
// The ESP32 starts its own access point so no router is required.
const char *AP_SSID_BASE = "Smart-Electrical-Fault";
String setupSSID = "Smart-Electrical-Fault-Wi-Fi-Setup";
const char *AP_PASSWORD = "E2KITIGA"; 
bool setupPortalMode = true;
String connectedSSID = "";
String deviceId = "";
String deviceHost = "Smart-Electrical-Fault";
bool mdnsStarted = false;
unsigned long wifiDisconnectedSince = 0;
unsigned long lastReconnectTry = 0;

const char *ADMIN_USER = "admin";
const char *ADMIN_PASSWORD = "Suraj.bh.com";

WebServer server(80);
Preferences prefs;
LiquidCrystal_I2C lcd(0x27, 20, 4); 

// ---------- Calibration ----------

float voltageCalibration = 2.00f; 
float voltageModuleRatio = 5.00f; 
float currentZeroVolts = 2.50f;  
float currentSensitivity = 0.100f; 
float currentCalibration = 1.00f;

float lowVoltageThreshold = 10.5f;
float highVoltageThreshold = 14.5f;
float overloadThreshold = 0.60f; 
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
bool relayEnabled = false; // Dashboard manual control; OFF after boot or any fault.
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
  // Average samples to reduce ADC noise. Calibrate against a multimeter.
  uint32_t sum = 0;
  constexpr int SAMPLE_COUNT = 12;

  for (int i = 0; i < SAMPLE_COUNT; ++i) {
    sum += analogRead(pin);
    delayMicroseconds(250);
  }

  const float rawAverage = static_cast<float>(sum) / SAMPLE_COUNT;
  return (rawAverage / 4095.0f) * 3.3f;
}

void addEvent(const String &message) {
  if (message == lastEvent && millis() - lastEventMs < 5000) {
    return;
  }

  lastEvent = message;
  lastEventMs = millis();
  history[historyIndex] = String(millis() / 1000) + "s: " + message;
  historyIndex = (historyIndex + 1) % 12;
  if (historyCount < 12) {
    ++historyCount;
  }

  // Persist only when an event changes, not on every sensor sample.
  prefs.begin("history", false);
  const int savedIndex = (historyIndex + 11) % 12;
  prefs.putString((String("e") + String(savedIndex)).c_str(), history[savedIndex]);
  prefs.putUInt("idx", static_cast<uint32_t>(historyIndex));
  prefs.putUInt("cnt", static_cast<uint32_t>(historyCount));
  prefs.end();
}

void loadPersistentHistory() {
  prefs.begin("history", true);
  historyIndex = static_cast<int>(prefs.getUInt("idx", 0) % 12);
  historyCount = constrain(static_cast<int>(prefs.getUInt("cnt", 0)), 0, 12);

  int validCount = 0;
  for (int i = 0; i < 12; ++i) {
    history[i] = prefs.getString((String("e") + String(i)).c_str(), "");
    if (history[i].length() > 0) {
      ++validCount;
    }
  }
  prefs.end();
  historyCount = min(historyCount, validCount);
}

void startDeviceMdns() {
  if (mdnsStarted || WiFi.status() != WL_CONNECTED) return;
  if (MDNS.begin(deviceHost.c_str())) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    Serial.print("Dashboard hostname: http://"); Serial.print(deviceHost); Serial.println(".local");
  }
}

// Assumes an ACTIVE-LOW relay module: LOW=ON, HIGH=OFF. Verify your module.
void setRelayOutput(bool on) {
  digitalWrite(PIN_RELAY, on ? LOW : HIGH);
  relayEnabled = on;
}
void setRelayOff() { setRelayOutput(false); }

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
  const String previousStatus = faultStatus;
  diagnoseRaw();
  const String detectedStatus = faultStatus;
  const String detectedSide = faultSide;

  if (testMode) {
    candidateFault = detectedStatus;
    candidateFaultSince = millis();
    return;
  }

  if (detectedStatus == previousStatus) {
    candidateFault = detectedStatus;
    candidateFaultSince = millis();
    return;
  }

  if (detectedStatus != candidateFault) {
    candidateFault = detectedStatus;
    candidateFaultSince = millis();
  }

  if (millis() - candidateFaultSince >= FAULT_CONFIRM_MS) {
    faultStatus = candidateFault;
    faultSide = detectedSide;
  } else {
    faultStatus = previousStatus;
  }
}

void updateIndicators() {
  const bool normal = faultStatus == "Normal";
  const bool critical = faultStatus.indexOf("Short Circuit") >= 0 ||
                        faultStatus == "High Voltage";
  const bool warning = !normal && !critical && faultStatus != "Starting";

  digitalWrite(PIN_GREEN, normal ? HIGH : LOW);
  digitalWrite(PIN_YELLOW, warning ? HIGH : LOW);
  digitalWrite(PIN_RED, critical ? HIGH : LOW);
  digitalWrite(PIN_BUZZER,
               (alarmsEnabled && buzzerEnabled && (warning || critical))
                   ? HIGH
                   : LOW);

  // Dashboard manual control is interlocked during faults and software tests.
  if (faultStatus != "Normal" || testMode) setRelayOff();
}

// HTML escaping is retained for HTML contexts only. JSON strings require JSON escaping.
String esc(const String &s) {
  String out;
  out.reserve(s.length() + 8);
  for (size_t i = 0; i < s.length(); ++i) {
    const char c = s[i];
    switch (c) {
      case '&': out += "&amp;"; break;
      case '<': out += "&lt;"; break;
      case '>': out += "&gt;"; break;
      case '\"': out += "&quot;"; break;
      case '\'': out += "&#39;"; break;
      default: out += c; break;
    }
  }
  return out;
}

String jsonEscape(const String &value) {
  String out;
  out.reserve(value.length() + 8);
  for (size_t i = 0; i < value.length(); ++i) {
    const unsigned char c = static_cast<unsigned char>(value[i]);
    switch (c) {
      case '\"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (c < 0x20) {
          const char hex[] = "0123456789abcdef";
          out += "\\u00";
          out += hex[(c >> 4) & 0x0F];
          out += hex[c & 0x0F];
        } else {
          out += static_cast<char>(c);
        }
        break;
    }
  }
  return out;
}

String dashboardHtml() {
  String h = R"HTML(
<!DOCTYPE html>
<html>
 <head>
  <meta content="width=device-width,initial-scale=1" name="viewport"/>
  <title>
   Smart IoT Fault System · Reliability Edition
  </title>
  <style>
   :root {
  color-scheme:dark;
  --bg:#070b17;
  --panel:#10182a;
  --line:#243451;
  --cyan:#38e8ff;
  --pink:#ff3bd4;
  --lime:#5cffb1;
  --text:#eef5ff;
  --muted:#94a8c9
}
* {
  box-sizing:border-box
}
body {
  margin:0;
  background:radial-gradient(ellipse at 10% 0%,#182448 0,transparent 40%),radial-gradient(ellipse at 90% 10%,#29133d 0,transparent 32%),var(--bg);
  color:var(--text);
  font:15px Inter,system-ui,Arial
}
header {
  padding:22px clamp(16px,4vw,42px);
  background:#0a1020d9;
  border-bottom:1px solid #2b4267;
  box-shadow:0 8px 40px #0005
}
h1 {
  font-size:clamp(21px,4vw,32px);
  letter-spacing:-.7px;
  margin:0 0 6px;
  background:linear-gradient(90deg,#fff,#60e9ff,#ff69df);
  background-clip:text;
  color:transparent
}
.muted {
  color:var(--muted);
  font-size:13px
}
.wrap {
  max-width:1250px;
  margin:auto;
  padding:clamp(12px,2vw,24px)
}
.nav {
  display:flex;
  gap:9px;
  flex-wrap:wrap;
  margin:14px 0 20px
}
.nav button,button {
  background:#141f36;
  color:#eaf4ff;
  border:1px solid #344a70;
  border-radius:12px;
  padding:11px 15px;
  cursor:pointer;
  transition:.2s;
  box-shadow:0 0 0 transparent
}
.nav button:hover,button:hover {
  border-color:var(--cyan);
  box-shadow:0 0 18px #38e8ff20;
  transform:translateY(-1px)
}
.nav button.active {
  background:linear-gradient(110deg,#154d68,#53265f);
  border-color:#53dff5
}
.grid {
  display:grid;
  grid-template-columns:repeat(auto-fit,minmax(155px,1fr));
  gap:13px
}
.card {
  background:linear-gradient(145deg,#121d32ed,#0d1425ed);
  border:1px solid #283a5a;
  border-radius:18px;
  padding:17px;
  box-shadow:0 12px 30px #0002;
  min-width:0
}
.label {
  font-size:12px;
  color:#9fb4d7;
  text-transform:uppercase;
  letter-spacing:1.3px
}
.value {
  font-size:clamp(24px,4vw,32px);
  font-weight:750;
  margin-top:8px;
  overflow-wrap:anywhere;
  font-variant-numeric:tabular-nums
}
.status {
  font-size:clamp(18px,3vw,24px);
  font-weight:750
}
.pill {
  display:inline-block;
  padding:6px 10px;
  border-radius:99px;
  background:#152943;
  color:#9eefff;
  border:1px solid #315477;
  font-size:12px
}
.section {
  display:none
}
.section.active {
  display:block
}
.row {
  display:flex;
  gap:10px;
  flex-wrap:wrap;
  align-items:center;
  margin:8px 0
}
input,select {
  background:#091122;
  color:#eef;
  border:1px solid #405171;
  padding:10px;
  border-radius:9px;
  max-width:100%
}
pre {
  white-space:pre-wrap;
  overflow-wrap:anywhere;
  color:#c9d8f3
}
.warn {
  color:#ffd166
}
.danger {
  color:#ff607c
}
.hero {
  display:flex;
  justify-content:space-between;
  gap:16px;
  align-items:center;
  flex-wrap:wrap;
  margin:4px 0 16px
}
.live {
  display:inline-flex;
  align-items:center;
  gap:8px;
  color:#7cffc5;
  font-size:12px;
  letter-spacing:1px
}
.dot {
  width:8px;
  height:8px;
  border-radius:50%;
  background:#5cffb1;
  box-shadow:0 0 14px #5cffb1;
  animation:pulse 1.6s infinite
}
@keyframes pulse {
  50% {
    opacity:.4;
    transform:scale(.8)
  }
}
.gauges {
  display:grid;
  grid-template-columns:repeat(auto-fit,minmax(190px,1fr));
  gap:15px;
  margin:14px 0
}
.gauge-card {
  text-align:center;
  padding:20px 12px
}
.gauge {
  --pct:0%;
  --gcolor:#5cffb1;
  width:150px;
  height:150px;
  max-width:100%;
  aspect-ratio:1;
  margin:15px auto 10px;
  border-radius:50%;
  position:relative;
  display:grid;
  place-items:center;
  background:conic-gradient(from 220deg,var(--gcolor) 0 var(--pct),#25334d var(--pct) 280deg,transparent 280deg 360deg);
  filter:drop-shadow(0 0 12px color-mix(in srgb,var(--gcolor),transparent 70%))
}
.gauge:before {
  content:'';
  position:absolute;
  inset:10px;
  border-radius:50%;
  background:radial-gradient(circle at 35% 25%,#172642,#090f1e 75%);
  border:1px solid #304361
}
.gauge-inner {
  z-index:1
}
.gauge-num {
  font-size:27px;
  font-weight:800;
  font-variant-numeric:tabular-nums
}
.gauge-unit {
  font-size:12px;
  color:#9db2d2
}
.gauge-caption {
  font-size:12px;
  color:#9db2d2;
  letter-spacing:1.3px;
  text-transform:uppercase
}
.fault-grid {
  display:grid;
  grid-template-columns:repeat(auto-fit,minmax(155px,1fr));
  gap:10px
}
.fault-tile {
  border:1px solid #293b58;
  background:#0d1526;
  border-radius:13px;
  padding:13px;
  display:flex;
  gap:10px;
  align-items:center;
  min-height:68px
}
.fault-lamp {
  width:11px;
  height:11px;
  border-radius:50%;
  background:#3e536d;
  flex-shrink:0
}
.fault-tile.active {
  border-color:#ff4f79;
  background:#301524;
  box-shadow:0 0 20px #ff286d2b;
  animation:faultBlink .85s infinite
}
.fault-tile.active .fault-lamp {
  background:#ff476f;
  box-shadow:0 0 13px #ff476f;
  animation:pulse .7s infinite
}
.fault-tile.ok {
  border-color:#235b50
}
.fault-tile.ok .fault-lamp {
  background:#55ffb2;
  box-shadow:0 0 10px #55ffb266
}
@keyframes faultBlink {
  50% {
    border-color:#ff9eaf;
    box-shadow:0 0 24px #ff286d55
  }
}
.toast-area {
  position:fixed;
  right:14px;
  top:14px;
  z-index:999;
  display:grid;
  gap:8px;
  width:min(360px,calc(100vw - 28px))
}
.toast {
  padding:14px 16px;
  border-radius:14px;
  background:#171e32;
  border:1px solid #ff527b;
  box-shadow:0 12px 40px #0009;
  animation:slidein .25s ease
}
.toast strong {
  color:#ff8ba6;
  display:block;
  margin-bottom:3px
}
@keyframes slidein {
  from {
    transform:translateX(20px);
    opacity:0
  }
  to {
    transform:none;
    opacity:1
  }
}
.section h2,.section h3 {
  letter-spacing:-.3px
}
.team-grid {
  display:grid;
  grid-template-columns:repeat(auto-fit,minmax(220px,1fr));
  gap:12px
}
.person {
  border:1px solid #2b3c5b;
  background:#0b1426;
  border-radius:14px;
  padding:15px
}
.person strong {
  display:block;
  color:#e9f8ff;
  margin-bottom:7px
}
.person .role {
  font-size:11px;
  letter-spacing:1px;
  color:#5fe6ff;
  text-transform:uppercase;
  margin-bottom:8px
}
canvas {
  max-width:100%
}
footer {
  color:#8499bc;
  font-size:12px;
  padding:22px 0
}
@media(max-width:540px) {
  .gauge {
    width:132px;
    height:132px
  }
  .card {
    padding:14px
  }
  .nav button {
    flex:1
  }
  .fault-grid {
    grid-template-columns:repeat(2,minmax(0,1fr))
  }
  .fault-tile {
    font-size:12px;
    padding:10px
  }
}
/* Premium responsive dashboard refinement */
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
  </style>
 </head>
 <body>
  <header>
   <h1>
    Smart IoT Electrical Fault Detection System
   </h1>
   <div class="muted">
    VERSION 1.0 · RELIABILITY + SMART WI-FI · 12V DC DEMONSTRATION
   </div>
  </header>
  <main class="wrap">
   <div class="nav">
    <button class="active" onclick="showTab('monitor',this)">
     Monitoring
    </button>
    <button onclick="showTab('about',this)">
     About Project
    </button>
    <button onclick="showTab('admin',this)">
     Admin Panel
    </button>
    <button onclick="window.location.href='/wifi'">
     Wi-Fi Settings
    </button>
   </div>
   <div class="system-strip">
    <div class="strip-item">
     <span class="strip-dot">
     </span>
     <span>
      ESP32 LOCAL LINK
     </span>
     <b id="linkState">
      CONNECTING
     </b>
    </div>
    <div class="strip-item">
     <span>
      ESP32 UPTIME
     </span>
     <b id="uptime">
      --
     </b>
    </div>
    <div class="strip-item">
     <span>
      LAST UPDATE
     </span>
     <b id="lastUpdate">
      --
     </b>
    </div>
    <div class="strip-item">
     <span>
      MODE
     </span>
     <b id="topMode">
      LIVE
     </b>
    </div>
   </div>
   <div aria-live="assertive" class="toast-area" id="toastArea">
   </div>
   <section class="section active" id="monitor">
    <div class="hero">
     <div>
      <div class="live">
       <span class="dot">
       </span>
       SYSTEM LIVE
      </div>
      <h2 style="margin:8px 0 4px">
       Electrical telemetry
      </h2>
      <div class="muted">
       Real-time DC supply and load health · three-colour diagnostics
      </div>
     </div>
     <div class="row">
      <button onclick="if('Notification' in window)Notification.requestPermission().then(x=&gt;showToast('Browser alerts','Permission: '+x));else showToast('Browser alerts','Notifications are not supported here.')">
       Enable notifications
      </button>
      <span class="pill" id="mode">
       Live sensor mode
      </span>
     </div>
    </div>
    <div class="card" style="margin:12px 0">
     <div class="hero">
      <div><h3 style="margin:0 0 5px">Manual Load Control</h3>
       <div class="muted">Switch the isolated 12V DC demonstration load using the relay.</div></div>
      <span class="pill" id="relayState">LOAD OFF</span>
     </div>
     <div class="row">
      <button id="relayOnBtn" onclick="setRelay(true)" type="button">LOAD ON</button>
      <button id="relayOffBtn" onclick="setRelay(false)" type="button">LOAD OFF</button>
     </div>
     <p class="muted" id="relayMessage">Admin login is required. Load ON is blocked during faults, test mode, or stale sensor data. Relay defaults OFF after restart.</p>
    </div>
    <div class="gauges">
     <div class="card gauge-card">
      <div class="gauge-caption">
       Input voltage
      </div>
      <div class="gauge" id="gVin">
       <div class="gauge-inner">
        <div class="gauge-num">
         <span id="vin">
          --
         </span>
        </div>
        <div class="gauge-unit">
         VOLTS
        </div>
       </div>
      </div>
      <div class="muted">
       Supply rail
      </div>
     </div>
     <div class="card gauge-card">
      <div class="gauge-caption">
       Output voltage
      </div>
      <div class="gauge" id="gVout">
       <div class="gauge-inner">
        <div class="gauge-num">
         <span id="vout">
          --
         </span>
        </div>
        <div class="gauge-unit">
         VOLTS
        </div>
       </div>
      </div>
      <div class="muted">
       Load rail
      </div>
     </div>
     <div class="card gauge-card">
      <div class="gauge-caption">
       Load current
      </div>
      <div class="gauge" id="gAmps">
       <div class="gauge-inner">
        <div class="gauge-num">
         <span id="amps">
          --
         </span>
        </div>
        <div class="gauge-unit">
         AMPERES
        </div>
       </div>
      </div>
      <div class="muted">
       Total measured current
      </div>
     </div>
    </div>
    <div class="grid">
     <div class="card">
      <div class="label">
       Estimated DC power
      </div>
      <div class="value">
       <span id="watts">
        --
       </span>
       <span style="font-size:14px;color:#9db2d2">
        W
       </span>
      </div>
     </div>
     <div class="card">
      <div class="label">
       Estimated DC energy
      </div>
      <div class="value">
       <span id="wh">
        --
       </span>
       <span style="font-size:14px;color:#9db2d2">
        Wh
       </span>
      </div>
     </div>
     <div class="card">
      <div class="label">
       Current system status
      </div>
      <div class="status" id="status" style="margin-top:9px">
       Connecting…
      </div>
      <p class="muted">
       Indicated section:
       <b id="side">
        --
       </b>
      </p>
     </div>
    </div>
    <div class="card" style="margin-top:15px">
     <div class="hero">
      <div>
       <h3 style="margin:0 0 5px">
        Network &amp; device
       </h3>
       <div class="muted">
        Per-board identity and local dashboard address
       </div>
      </div>
      <a href="/wifi">
       Manage Wi-Fi
      </a>
     </div>
     <div class="grid">
      <div>
       <div class="label">
        Device ID
       </div>
       <div class="value" id="deviceIdDash" style="font-size:19px">
        --
       </div>
      </div>
      <div>
       <div class="label">
        Network
       </div>
       <div class="value" id="wifiSsidDash" style="font-size:19px">
        --
       </div>
      </div>
      <div>
       <div class="label">
        Dashboard IP
       </div>
       <div class="value" id="wifiIpDash" style="font-size:19px">
        --
       </div>
      </div>
      <div>
       <div class="label">
        Signal / Link
       </div>
       <div class="value" id="wifiSignalDash" style="font-size:19px">
        --
       </div>
      </div>
     </div>
     <p class="muted">
      If router connection fails, reconnect to SmartFault-Setup and open the Wi-Fi manager.
     </p>
    </div>
    <div class="card" style="margin-top:15px">
     <h3>
      Sensor health &amp; diagnostics
     </h3>
     <p class="muted" id="sensorHealth">
      Waiting for sensor data…
     </p>
     <p id="diagnosis">
      Waiting for diagnostic status…
     </p>
     <p class="muted">
      Sensor status is a plausibility check, not proof that a sensor is electrically healthy.
     </p>
    </div>
    <div class="card" style="margin-top:15px">
     <div class="hero">
      <div>
       <h3 style="margin:0 0 5px">
        Fault matrix
       </h3>
       <div class="muted">
        A fault tile blinks when the system reports that condition.
       </div>
      </div>
      <span class="pill" id="faultSummary">
       No active fault
      </span>
     </div>
     <div class="fault-grid">
      <div class="fault-tile" id="fault-short">
       <span class="fault-lamp">
       </span>
       <span>
        <b>
         Short Circuit Suspected
        </b>
        <br/>
        <span class="muted">
         Output/load
        </span>
       </span>
      </div>
      <div class="fault-tile" id="fault-open">
       <span class="fault-lamp">
       </span>
       <span>
        <b>
         Open Circuit Suspected
        </b>
        <br/>
        <span class="muted">
         Output/load
        </span>
       </span>
      </div>
      <div class="fault-tile" id="fault-over">
       <span class="fault-lamp">
       </span>
       <span>
        <b>
         Overload
        </b>
        <br/>
        <span class="muted">
         Current high
        </span>
       </span>
      </div>
      <div class="fault-tile" id="fault-low">
       <span class="fault-lamp">
       </span>
       <span>
        <b>
         Low Voltage
        </b>
        <br/>
        <span class="muted">
         Supply low
        </span>
       </span>
      </div>
      <div class="fault-tile" id="fault-high">
       <span class="fault-lamp">
       </span>
       <span>
        <b>
         High Voltage
        </b>
        <br/>
        <span class="muted">
         Supply high
        </span>
       </span>
      </div>
      <div class="fault-tile ok" id="fault-normal">
       <span class="fault-lamp">
       </span>
       <span>
        <b>
         Normal Operation
        </b>
        <br/>
        <span class="muted">
         No detected fault
        </span>
       </span>
      </div>
     </div>
    </div>
    <div class="card" style="margin-top:15px">
     <h3>
      Live trend · last 60 samples
     </h3>
     <canvas height="220" id="trend" style="width:100%;height:auto;background:#091122;border-radius:12px" width="900">
     </canvas>
     <div class="muted">
      Output voltage and current trend. Values are for demonstration, not a certified protection instrument.
     </div>
    </div>
    <div class="card" style="margin-top:15px">
     <h3>
      Recent system events
     </h3>
     <pre id="history">Loading…</pre>
    </div>
   </section>
   <section class="section" id="about">
    <div class="card">
     <div class="live">
      <span class="dot">
      </span>
      TEAM &amp; PROJECT
     </div>
     <h2>
      About the project
     </h2>
     <p>
      This project demonstrates local IoT monitoring of an isolated 12V DC test circuit using ESP32, voltage sensors and a current sensor. It reports measurements and indicates suspected abnormal conditions on the LCD and a live dashboard.
     </p>
     <h3>
      Team
     </h3>
     <p class="muted">
      Team project · Second Year Electrician Trade
     </p>
     <h3>
      Contributions for this project
     </h3>
     <div class="team-grid">
      <div class="person">
       <div class="role">
        Components &amp; Hardware
       </div>
       <strong>
        Soupitk Santra
       </strong>
       Led component purchasing and sourcing, helped arrange required parts, and worked on hardware connections and assembly.
      </div>
      <div class="person">
       <div class="role">
        Programming &amp; Integration
       </div>
       <strong>
        Suraj Datta
       </strong>
       Designed and implemented the project coding and programming, helped prepare the item-wise component list, and assisted with circuit connections and integration.
      </div>
      <div class="person">
       <div class="role">
        Component Research
       </div>
       <strong>
        Prabhas Bhunia
       </strong>
       Helped identify required components, compare options, and coordinate component sourcing and project preparation.
      </div>
      <div class="person">
       <div class="role">
        Assembly Support
       </div>
       <strong>
        Devkrishna Kanta Samai
       </strong>
       Supported component planning, hardware preparation, and assembly tasks during the project build.
      </div>
      <div class="person">
       <div class="role">
        Build &amp; Testing Support
       </div>
       <strong>
        Surajit Samai
       </strong>
       Assisted with component coordination, wiring support, checking connections, and preparing the demonstration setup.
      </div>
     </div>
     <p class="muted" style="margin-top:14px">
      Contribution descriptions summarize the team's stated work and should be adjusted if the actual task split changes.
     </p>
     <h3>
      System limitations
     </h3>
     <p>
      Fault identification is indicative, not certified protection. Sensor-health flags are plausibility checks and cannot prove sensor integrity. One current sensor measures total current and cannot identify an individual parallel branch. Power factor is not measured in this DC project; energy is estimated.
     </p>
     <p class="pill">
      Approximate project budget: ₹4,500
     </p>
    </div>
   </section>
   <section class="section" id="admin">
    <div class="card">
     <h2>
      Admin Panel
     </h2>
     <p class="muted">
      Browser will ask for the admin username and password.
     </p>
     <button onclick="loadAdmin()">
      Unlock / Load Settings
     </button>
     <div id="adminBox" style="margin-top:14px">
     </div>
    </div>
   </section>
   <footer>
    V 1.0 Reliability Edition · Use only with an isolated 12V DC supply. Never connect to household AC mains. Do not physically short the supply. Fault indications are advisory, not certified protection.
   </footer>
  </main>
  <script>
   function showTab(id,b) {
  document.querySelectorAll('.section').forEach(x=>x.classList.remove('active'));
  document.getElementById(id).classList.add('active');
  document.querySelectorAll('.nav button').forEach(x=>x.classList.remove('active'));
  b.classList.add('active')
}

  let previousFault='',lastNotifyAt=0,lastSuccessAt=0;
function diagnosisText(status,side,simulated) {
  if(simulated)return 'Software simulation active: '+status+'. No physical fault has been created.';
  const advice= {
    'Normal':'No fault is currently indicated. Verify readings against a multimeter.','Low Voltage':'Check the isolated DC adapter, wiring, fuse and terminal tightness with power safely removed.','High Voltage':'Switch off the isolated low-voltage supply and verify adapter output with a multimeter.','Overload':'Remove power before checking the load rating, wiring and sensor calibration.','Short Circuit Suspected':'Do not create a real short. Remove power and inspect the load/wiring for accidental shorts.','Open Circuit Suspected':'Remove power and inspect disconnected terminals, fuse continuity and load wiring.'
  };
  return (advice[status]||'Check sensor readings and calibration.')+' Monitored section: '+(side||'not identified')+'.';

}
function setGauge(id,val,max,low,high) {
  let el=document.getElementById(id);
  if(!el)return;
  let n=Number(val);
  let pct=Math.max(0,Math.min(1,(Number.isFinite(n)?n:0)/max));
  let col=(!Number.isFinite(n)||n<0)?'#94a8c9':(n<low||n>high)?'#ff476f':(n<low*1.1||n>high*.9)?'#ffe16b':'#5cffb1';
  el.style.setProperty('--pct',(pct*280)+'deg');
  el.style.setProperty('--gcolor',col)
}
function showToast(title,msg) {
  let area=document.getElementById('toastArea');
  let t=document.createElement('div');
  t.className='toast';
  t.innerHTML='<strong>⚠ '+title+'</strong><span>'+msg.replace(/[<>]/g,'')+'</span>';
  area.prepend(t);
  setTimeout(()=>t.remove(),6500)
}
function updateFaultTiles(status) {
  let map= {
    'fault-short':status.includes('Short'),'fault-open':status.includes('Open'),'fault-over':status==='Overload','fault-low':status==='Low Voltage','fault-high':status==='High Voltage','fault-normal':status==='Normal'
  };
  Object.keys(map).forEach(id=> {
    let el=document.getElementById(id);
    el.classList.toggle('active',!!map[id]);
    el.classList.toggle('ok',id==='fault-normal'&&!!map[id])
  });
  document.getElementById('faultSummary').textContent=status==='Normal'?'No active fault':'ACTIVE: '+status
}
async function setRelay(on) {
  const message=document.getElementById('relayMessage');
  const onBtn=document.getElementById('relayOnBtn');
  const offBtn=document.getElementById('relayOffBtn');
  onBtn.disabled=true; offBtn.disabled=true;
  try {
    const r=await fetch('/relay',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({on:!!on})});
    const d=await r.json();
    if(!r.ok || !d.ok) throw Error(d.error||('HTTP '+r.status));
    document.getElementById('relayState').textContent=d.relayOn?'LOAD ON':'LOAD OFF';
    message.textContent=d.relayOn?'Load switched ON.':'Load switched OFF.';
  } catch(e) {
    message.textContent='Relay command failed: '+e.message+' (Admin login may be required.)';
  } finally { onBtn.disabled=false; offBtn.disabled=false; update(); }
}

async function update() {
  try {
    let r=await fetch('/api', {
      cache:'no-store'
    });
    if(!r.ok)throw Error('HTTP '+r.status);
    let d=await r.json();
    lastSuccessAt=Date.now();
    document.getElementById('linkState').style.color='';
    ['vin','vout','amps','watts','wh'].forEach(k=> {
      const el=document.getElementById(k);
      const n=Number(d[k]);
      el.textContent=Number.isFinite(n)?n.toFixed(2):'--';

    });
    setGauge('gVin',d.vin,16,Number(d.thresholds?.lowV??10.5),Number(d.thresholds?.highV??14.5));
    setGauge('gVout',d.vout,16,Number(d.thresholds?.lowV??10.5),Number(d.thresholds?.highV??14.5));
    setGauge('gAmps',d.amps,2,0,Number(d.thresholds?.overA??0.6));
    let s=document.getElementById('status');
    s.textContent=d.status+' — '+d.side;
    const relayStateEl=document.getElementById('relayState');
    if(relayStateEl)relayStateEl.textContent=d.relayOn?'LOAD ON':'LOAD OFF';
    const relayOnBtn=document.getElementById('relayOnBtn');
    if(relayOnBtn)relayOnBtn.disabled=d.status!=='Normal'||d.testMode||Number(d.sampleAgeMs||0)>2500;
    s.className='status '+(d.status==='Normal'?'':(d.status.includes('Short')||d.status.includes('High')?'danger':'warn'));
    document.getElementById('mode').textContent=d.testMode?'SOFTWARE TEST MODE: '+d.testFault:'Live sensor mode';
    document.getElementById('topMode').textContent=d.testMode?'DEMO':'LIVE';
    document.getElementById('side').textContent=d.side||'None';
    document.getElementById('linkState').textContent=d.wifiConnected?'ONLINE':'SETUP / RECONNECTING';
    if(document.getElementById('deviceIdDash'))document.getElementById('deviceIdDash').textContent=d.deviceId||'--';
    if(document.getElementById('wifiSsidDash'))document.getElementById('wifiSsidDash').textContent=d.wifiSSID||'--';
    if(document.getElementById('wifiIpDash'))document.getElementById('wifiIpDash').textContent=d.wifiIP||'--';
    if(document.getElementById('wifiSignalDash'))document.getElementById('wifiSignalDash').textContent=d.wifiConnected?(Number(d.wifiRSSI)+' dBm'):'Setup mode';
    document.getElementById('uptime').textContent=Math.floor(Number(d.uptimeMs||0)/1000)+'s';
    document.getElementById('lastUpdate').textContent=new Date().toLocaleTimeString();
    if(Number(d.sampleAgeMs||0)>2500) {
      document.getElementById('linkState').textContent='DATA STALE';
      document.getElementById('linkState').style.color='#ffd166';

    }

let health=d.sensorHealth|| {

    };
    let healthEl=document.getElementById('sensorHealth');
    if(healthEl)healthEl.textContent='Input: '+(health.inputVoltage||'UNKNOWN')+' · Output: '+(health.outputVoltage||'UNKNOWN')+' · Current: '+(health.current||'UNKNOWN');
    let diag=document.getElementById('diagnosis');
    if(diag)diag.textContent=diagnosisText(d.status,d.side,d.testMode);
    document.getElementById('history').textContent=(d.history||[]).join('\n')||'No events yet';
    updateFaultTiles(d.status);
    if(previousFault&&d.status!==previousFault&&d.status!=='Normal') {
      showToast('FAULT ALERT',d.status+' · '+d.side);
      if('Notification'in window&&Notification.permission==='granted')new Notification('Smart Fault System: '+d.status, {
        body:'Indicated section: '+d.side
      });

    }
    if(d.status==='Normal'&&previousFault&&previousFault!=='Normal')showToast('System recovered','Monitoring reports normal operation.');
    previousFault=d.status;
    drawTrend(d.trendVout,d.trendCurrent);

  }
  catch(e) {
    document.getElementById('status').textContent='Connection lost — readings unavailable';
    ['vin','vout','amps','watts','wh'].forEach(k=> {
      const el=document.getElementById(k);
      if(el)el.textContent='--';

    });
    document.getElementById('side').textContent='Unavailable';
    document.getElementById('sensorHealth').textContent='No fresh sensor data';
    document.getElementById('faultSummary').textContent='DATA UNAVAILABLE';
    document.getElementById('linkState').textContent='OFFLINE';
    document.getElementById('linkState').style.color='#ff607c';

  }

}
document.addEventListener('click',e=> {
  if(e.target&&e.target.textContent&&e.target.textContent.includes('Enable alerts')&&'Notification'in window)Notification.requestPermission();

});

  function drawTrend(v,c) {
  const canvas=document.getElementById('trend');
  if(!canvas)return;
  const ctx=canvas.getContext('2d');
  const w=canvas.width,h=canvas.height;
  ctx.clearRect(0,0,w,h);
  ctx.strokeStyle='#2b3b59';
  ctx.lineWidth=1;
  for(let i=1;
  i<5;
  i++) {
    ctx.beginPath();
    ctx.moveTo(0,h*i/5);
    ctx.lineTo(w,h*i/5);
    ctx.stroke()
  }
  function line(a,max) {
    if(!a||a.length<2)return;
    ctx.beginPath();
    ctx.strokeStyle='#72b7ff';
    a.forEach((x,i)=> {
      let px=i*w/(a.length-1),py=h-12-Math.max(0,Math.min(1,x/(max||1)))*(h-24);
      if(i===0)ctx.moveTo(px,py);
      else ctx.lineTo(px,py)
    });
    ctx.stroke()
  }
  line(v,16);
  ctx.strokeStyle='#ffc66d';
  if(c&&c.length>1) {
    ctx.beginPath();
    c.forEach((x,i)=> {
      let px=i*w/(c.length-1),py=h-12-Math.max(0,Math.min(1,x/3))*(h-24);
      if(i===0)ctx.moveTo(px,py);
      else ctx.lineTo(px,py)
    });
    ctx.stroke()
  }

}

  async function loadAdmin() {
  try {
    let r=await fetch('/admin/settings');
    if(!r.ok)throw Error('Authentication failed');
    let d=await r.json();
    document.getElementById('adminBox').innerHTML=`<div class="row">Low V <input id="low" type="number" step="0.1" value="${d.low}"> High V <input id="high" type="number" step="0.1" value="${d.high}"></div><div class="row">Overload A <input id="over" type="number" step="0.05" value="${d.over}"> Short suspected A <input id="short" type="number" step="0.1" value="${d.short}"></div><div class="row">Voltage module ratio <input id="ratio" type="number" step="0.1" value="${d.ratio}"> Voltage calibration <input id="vcal" type="number" step="0.01" value="${d.vcal}"></div><div class="row">Current zero V <input id="zero" type="number" step="0.01" value="${d.zero}"> Current sensitivity V/A <input id="sens" type="number" step="0.001" value="${d.sens}"></div><div class="row"><label><input id="alarm" type="checkbox" ${d.alarm?'checked':''}> Alarms enabled</label><label><input id="buzz" type="checkbox" ${d.buzz?'checked':''}> Buzzer enabled</label></div><div class="row"><select id="test"><option>Normal</option><option>Low Voltage</option><option>High Voltage</option><option>Overload</option><option>Open Circuit Suspected</option><option>Short Circuit Suspected</option></select><button onclick="setTest()">Apply software test</button><button onclick="saveSettings()">Save settings</button><button onclick="resetEnergy()">Reset energy</button><button onclick="restartBoard()">Restart ESP32</button><button onclick="downloadEvents()">Export event log CSV</button></div><p class="muted">Use software test mode only; it does not create a physical electrical fault.</p>`;

  }
  catch(e) {
    document.getElementById('adminBox').textContent='Could not load settings. Check credentials and try again.';

  }

}

  async function saveSettings() {
  let p= {
    low:+low.value,high:+high.value,over:+over.value,short:+short.value,ratio:+ratio.value,vcal:+vcal.value,zero:+zero.value,sens:+sens.value,alarm:alarm.checked,buzz:buzz.checked
  };
  let r=await fetch('/admin/settings', {
    method:'POST',headers: {
      'Content-Type':'application/json'
    },body:JSON.stringify(p)
  });
  alert(r.ok?'Settings saved':'Save failed');

}

  async function setTest() {
  let r=await fetch('/admin/test?fault='+encodeURIComponent(test.value), {
    method:'POST'
  });
  alert(r.ok?'Test mode updated':'Update failed');

}

  async function resetEnergy() {
  let r=await fetch('/admin/energy-reset', {
    method:'POST'
  });
  alert(r.ok?'Energy counter reset':'Reset failed');

}

  async function restartBoard() {
  if(confirm('Restart ESP32?')) {
    await fetch('/admin/restart', {
      method:'POST'
    });

  }

}

  function downloadEvents() {
  window.location.href='/admin/events.csv';

}

  setInterval(update,1000);
update();
  </script>
 </body>
</html>

)HTML";
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
    s += "\"" + jsonEscape(history[idx]) + "\"";
  }
  s += "]";
  return s;
}

void handleRelayControl() {
  if (!requireAdmin()) return;
  if (!server.hasArg("plain")) {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Missing request body\"}");
    return;
  }
  String body = server.arg("plain");
  const bool turnOn = body.indexOf("\"on\":true") >= 0;
  if (!turnOn) {
    setRelayOff();
    addEvent("Dashboard commanded load OFF");
    server.send(200, "application/json", "{\"ok\":true,\"relayOn\":false}");
    return;
  }
  const bool freshData = lastSampleMs != 0 && millis() - lastSampleMs <= 2500;
  const bool validVoltage = inputVoltage >= 5.0f && inputVoltage <= 30.0f &&
                            outputVoltage >= 0.15f && outputVoltage <= 30.0f;
  if (!freshData || !validVoltage || faultStatus != "Normal" || candidateFault != "Normal" || testMode) {
    setRelayOff();
    server.send(409, "application/json", "{\"ok\":false,\"relayOn\":false,\"error\":\"Load ON blocked: check live readings, fault status and test mode\"}");
    return;
  }
  setRelayOutput(true);
  addEvent("Dashboard commanded load ON");
  server.send(200, "application/json", "{\"ok\":true,\"relayOn\":true}");
}

void sendApi() {
  String json = "{";
  json += "\"vin\":" + String(inputVoltage, 3);
  json += ",\"vout\":" + String(outputVoltage, 3);
  json += ",\"amps\":" + String(currentAmps, 3);
  json += ",\"watts\":" + String(powerWatts, 3);
  json += ",\"wh\":" + String(energyWh, 4);
  json += ",\"status\":\"" + jsonEscape(faultStatus) + "\"";
  json += ",\"relayOn\":" + String(relayEnabled ? "true" : "false");
  json += ",\"side\":\"" + jsonEscape(faultSide) + "\"";
  json += ",\"testMode\":" + String(testMode ? "true" : "false");
  json += ",\"testFault\":\"" + jsonEscape(testFault) + "\"";
  json += ",\"uptimeMs\":" + String(millis());
  json += ",\"sampleAgeMs\":" + String(lastSampleMs == 0 ? 0 : millis() - lastSampleMs);
  json += ",\"deviceId\":\"" + jsonEscape(deviceId) + "\"";
  json += ",\"hostname\":\"" + jsonEscape(deviceHost + ".local") + "\"";
  json += ",\"wifiConnected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  json += ",\"wifiSSID\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.SSID() : setupSSID) + "\"";
  json += ",\"wifiIP\":\"" + jsonEscape(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString()) + "\"";
  json += ",\"wifiRSSI\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  json += ",\"setupAP\":" + String(WiFi.softAPIP().toString() != "0.0.0.0" ? "true" : "false");
  json += ",\"freeHeap\":" + String(ESP.getFreeHeap());

  json += ",\"thresholds\":{";
  json += "\"lowV\":" + String(lowVoltageThreshold, 2);
  json += ",\"highV\":" + String(highVoltageThreshold, 2);
  json += ",\"overA\":" + String(overloadThreshold, 2);
  json += ",\"shortA\":" + String(shortCurrentThreshold, 2) + "}";

  const bool inputNeedsCheck = inputVoltage < 0.15f || inputVoltage > 30.0f;
  const bool outputNeedsCheck = outputVoltage < 0.15f || outputVoltage > 30.0f;
  const bool currentNeedsCheck = currentAmps >= 24.9f;
  json += ",\"sensorHealth\":{";
  json += "\"inputVoltage\":\"" + String(inputNeedsCheck ? "CHECK" : "READING") + "\"";
  json += ",\"outputVoltage\":\"" + String(outputNeedsCheck ? "CHECK" : "READING") + "\"";
  json += ",\"current\":\"" + String(currentNeedsCheck ? "CHECK" : "READING") + "\"}";
  json += ",\"history\":" + historyJson();

  json += ",\"trendVout\":[";
  for (int i = 0; i < chartCount; ++i) {
    if (i > 0) json += ",";
    const int index = (chartIndex - chartCount + i + 60) % 60;
    json += String(chartVout[index], 2);
  }
  json += "],\"trendCurrent\":[";
  for (int i = 0; i < chartCount; ++i) {
    if (i > 0) json += ",";
    const int index = (chartIndex - chartCount + i + 60) % 60;
    json += String(chartCurrent[index], 2);
  }
  json += "]}";

  server.send(200, "application/json; charset=utf-8", json);
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
  if (!requireAdmin()) {
    return;
  }

  const String body = server.arg("plain");
  if (body.length() == 0 || body.length() > 1024 || body[0] != '{') {
    server.send(400, "application/json", "{\"ok\":false,\"error\":\"Invalid settings payload\"}");
    return;
  }

  auto getNumber = [&](const char *key, float fallback) -> float {
    const String needle = String("\"") + key + "\":";
    int start = body.indexOf(needle);
    if (start < 0) {
      return fallback;
    }

    start += needle.length();
    while (start < static_cast<int>(body.length()) &&
           (body[start] == ' ' || body[start] == '\t')) {
      ++start;
    }

    int finish = start;
    while (finish < static_cast<int>(body.length()) &&
           String("0123456789.-").indexOf(body[finish]) >= 0) {
      ++finish;
    }
    if (finish == start) {
      return fallback;
    }
    return body.substring(start, finish).toFloat();
  };

  lowVoltageThreshold = constrain(getNumber("low", lowVoltageThreshold), 0.0f, 12.0f);
  highVoltageThreshold = constrain(getNumber("high", highVoltageThreshold), 12.1f, 25.0f);
  overloadThreshold = constrain(getNumber("over", overloadThreshold), 0.05f, 2.0f);
  shortCurrentThreshold = constrain(getNumber("short", shortCurrentThreshold), overloadThreshold, 3.0f);
  voltageModuleRatio = constrain(getNumber("ratio", voltageModuleRatio), 0.1f, 10.0f);
  voltageCalibration = constrain(getNumber("vcal", voltageCalibration), 0.1f, 5.0f);
  currentZeroVolts = constrain(getNumber("zero", currentZeroVolts), 0.5f, 3.0f);
  currentSensitivity = constrain(getNumber("sens", currentSensitivity), 0.02f, 0.2f);
  alarmsEnabled = body.indexOf("\"alarm\":true") >= 0;
  buzzerEnabled = body.indexOf("\"buzz\":true") >= 0;

  prefs.begin("faultsys", false);
  prefs.putFloat("low", lowVoltageThreshold);
  prefs.putFloat("high", highVoltageThreshold);
  prefs.putFloat("over", overloadThreshold);
  prefs.putFloat("short", shortCurrentThreshold);
  prefs.putFloat("ratio", voltageModuleRatio);
  prefs.putFloat("vcal", voltageCalibration);
  prefs.putFloat("zero", currentZeroVolts);
  prefs.putFloat("sens", currentSensitivity);
  prefs.putBool("alarm", alarmsEnabled);
  prefs.putBool("buzz", buzzerEnabled);
  prefs.end();

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
  if (millis() - lastLcdMs < 2500) {
    return;
  }
  lastLcdMs = millis();
  lcd.clear();

  if (millis() - bootMs < 10000) {
    lcd.setCursor(0, 0);
    lcd.print("Smart IoT Fault V6.1");
    lcd.setCursor(0, 1);

    if (millis() - bootMs < 2000) {
      lcd.print("Welcome to System");
    } else if (millis() - bootMs < 5000) {
      lcd.print("Countdown: ");
      lcd.print(3 - ((millis() - bootMs - 2000) / 1000));
    } else if (millis() - bootMs < 8000) {
      lcd.print("Initializing...");
    } else {
      lcd.print("WiFi: AP Ready");
    }

    lcd.setCursor(0, 2);
    lcd.print(WiFi.status() == WL_CONNECTED ? connectedSSID.substring(0, 20) : setupSSID);
    lcd.setCursor(0, 3);
    lcd.print(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
    return;
  }

  if (lcdPage == 0) {
    lcd.setCursor(0, 0);
    lcd.print("In:"); lcd.print(inputVoltage, 1); lcd.print("V Out:");
    lcd.print(outputVoltage, 1); lcd.print("V");
    lcd.setCursor(0, 1);
    lcd.print("Current: "); lcd.print(currentAmps, 2); lcd.print(" A");
    lcd.setCursor(0, 2);
    lcd.print("Power: "); lcd.print(powerWatts, 1); lcd.print(" W");
    lcd.setCursor(0, 3);
    lcd.print("Energy: "); lcd.print(energyWh, 2); lcd.print(" Wh");
  } else {
    lcd.setCursor(0, 0); lcd.print("FAULT STATUS:");
    lcd.setCursor(0, 1); lcd.print(faultStatus.substring(0, 20));
    lcd.setCursor(0, 2); lcd.print("Side:"); lcd.print(faultSide.substring(0, 14));
    lcd.setCursor(0, 3);
    lcd.print(WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString());
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

String wifiManagerHtml() {
  String html = R"HTML(
<!DOCTYPE html>
<html>
 <head>
  <meta content="width=device-width,initial-scale=1" name="viewport"/>
  <meta content="#08111f" name="theme-color"/>
  <title>
   SmartFault Wi-Fi Setup
  </title>
  <style>
   * {
  box-sizing:border-box
}
body {
  margin:0;
  background:radial-gradient(circle at top,#163253,#070b13 60%);
  color:#edf5ff;
  font:16px system-ui,Arial
}
main {
  max-width:760px;
  margin:auto;
  padding:22px
}
.card {
  background:#101c2d;
  border:1px solid #2b4260;
  border-radius:20px;
  padding:20px;
  margin:14px 0;
  box-shadow:0 15px 40px #0005
}
h1 {
  font-size:clamp(25px,5vw,36px);
  margin:6px 0
}
p {
  color:#aebed3;
  line-height:1.55
}
.pill {
  display:inline-block;
  background:#163a37;
  color:#70f0c1;
  border-radius:30px;
  padding:6px 11px;
  font-size:12px
}
button,input {
  font:inherit;
  border-radius:11px;
  padding:12px;
  border:1px solid #334d6c;
  background:#0a1423;
  color:#f3f7ff;
  width:100%;
  margin:5px 0
}
button {
  background:linear-gradient(120deg,#00c9a7,#3d8bff);
  color:#04101d;
  font-weight:800;
  border:0;
  cursor:pointer
}
.network {
  display:flex;
  align-items:center;
  gap:12px;
  padding:12px 4px;
  border-bottom:1px solid #2b3c52
}
.network button {
  width:auto;
  min-width:95px;
  margin-left:auto
}
.netinfo {
  min-width:0;
  flex:1
}
.netinfo strong {
  display:block;
  overflow-wrap:anywhere
}
.signal {
  height:5px;
  background:#26354a;
  border-radius:9px;
  overflow:hidden;
  margin-top:7px;
  max-width:180px
}
.signal span {
  display:block;
  height:100%;
  background:linear-gradient(90deg,#f7b955,#50e6b2);
  border-radius:9px
}
button:focus-visible,input:focus-visible {
  outline:2px solid #60e9ff;
  outline-offset:2px
}
button:disabled {
  opacity:.55;
  cursor:wait
}
.muted {
  font-size:13px;
  color:#9cafc6
}
.row {
  display:grid;
  grid-template-columns:1fr 1fr;
  gap:10px
}
@media(max-width:520px) {
  .row {
    grid-template-columns:1fr
  }
  main {
    padding:14px
  }
  .card {
    padding:15px
  }
}
a {
  color:#6fe6ff
}
  </style>
 </head>
 <body>
  <main>
   <span class="pill">
    ESP32 LOCAL SETUP
   </span>
   <h1>
    Connect your ESP32 to Wi-Fi
   </h1>
   <p>
    Connect to this ESP32's unique setup network first. Select a nearby Wi-Fi network or enter its name manually. Internet is not required for the initial setup.
   </p>
   <div class="card">
    <div class="hero">
     <div>
      <h2>
       1. Nearby networks
      </h2>
      <p class="muted">
       Choose a network with a strong signal. ESP32 supports 2.4 GHz Wi-Fi.
      </p>
     </div>
     <span class="pill" id="scanState">
      READY TO SCAN
     </span>
    </div>
    <div class="row">
     <button onclick="scan()" type="button">
      Scan / Refresh Networks
     </button>
     <input aria-label="Filter network names" id="netFilter" oninput="renderNetworks()" placeholder="Filter network names"/>
    </div>
    <div class="muted" id="nets">
     Tap scan to search for nearby Wi-Fi networks.
    </div>
   </div>
   <div class="card">
    <h2>
     2. Network credentials
    </h2>
    <form id="wf">
     <label>
      Wi-Fi name (SSID)
     </label>
     <input autocomplete="off" id="ssid" maxlength="32" name="ssid" placeholder="Select a network or type its name" required=""/>
     <label>
      Wi-Fi password
     </label>
     <input autocomplete="new-password" id="pass" maxlength="63" name="pass" placeholder="Password (leave blank for open network)" type="password"/>
     <label>
      <input id="show" onchange="document.getElementById('pass').type=this.checked?'text':'password'" style="width:auto" type="checkbox"/>
      Show password
     </label>
     <button type="submit">
      Save and connect
     </button>
    </form>
    <p class="muted">
     The ESP32 will restart and try to connect. If it cannot connect, the setup Wi-Fi will return so you can try again.
    </p>
   </div>
   <div class="card">
    <h2>
     Manage saved Wi-Fi
    </h2>
    <p class="muted">
     Forget the stored router network to return to setup mode. When connected through your router, this action asks for admin credentials.
    </p>
    <button onclick="forgetWifi()" type="button">
     Forget saved Wi-Fi &amp; restart
    </button>
   </div>
   <div class="card">
    <h2>
     Device &amp; connection status
    </h2>
    <div class="row">
     <div>
      <span class="muted">
       Device ID
      </span>
      <p id="deviceId">
       Loading…
      </p>
     </div>
     <div>
      <span class="muted">
       Current mode
      </span>
      <p id="deviceMode">
       Setup access point
      </p>
     </div>
    </div>
    <p class="muted" id="connectionDetails">
     Reading connection status…
    </p>
    <button onclick="refreshStatus()" type="button">
     Refresh status
    </button>
   </div>
   <div class="card">
    <h2>
     Connection help
    </h2>
    <p id="msg">
     Setup network:
     <b>
      Smart-Electrical-Fault-Wi-Fi-Setup
     </b>
     <br/>
     Setup password:
     <b>
      *****
     </b>
     <br/>
     Setup address:
     <b>
      http://192.168.4.1
     </b>
    </p>
    <p class="muted">
     After connecting to your router, join the same Wi-Fi on your phone/laptop and open the ESP32 IP shown on its LCD or Serial Monitor. Some routers isolate connected devices and may block this.
    </p>
   </div>
  </main>
  <script>
   let networks=[];

function signalLabel(r) {
  return r>=-55?'Excellent':r>=-67?'Good':r>=-75?'Fair':'Weak'
}

function renderNetworks() {
  const n=document.getElementById('nets');
  const q=(document.getElementById('netFilter').value||'').toLowerCase();
  const rows=networks.filter(x=>x.ssid.toLowerCase().includes(q));
  n.innerHTML='';
  if(!rows.length) {
    n.textContent=networks.length?'No networks match this filter.':'No networks found. Type the SSID manually.';
    return
  }

rows.forEach(x=> {
    let d=document.createElement('div');
    d.className='network';
    let info=document.createElement('div');
    info.className='netinfo';
    let title=document.createElement('strong');
    title.textContent=x.ssid;
    let sub=document.createElement('div');
    sub.className='muted';
    sub.textContent=signalLabel(x.rssi)+' · '+x.rssi+' dBm · '+(x.secure?'Password required':'Open network');
    let meter=document.createElement('div');
    meter.className='signal';
    let fill=document.createElement('span');
    fill.style.width=Math.max(8,Math.min(100,(x.rssi+100)*2))+'%';
    meter.append(fill);
    info.append(title,sub,meter);
    let b=document.createElement('button');
    b.textContent='Select';
    b.onclick=()=> {
      document.getElementById('ssid').value=x.ssid;
      document.getElementById('pass').focus();
      document.getElementById('msg').textContent='Selected '+x.ssid+'. Enter its password, then Save and connect.'
    };
    d.append(info,b);
    n.append(d)
  })
}

async function scan() {
  let n=document.getElementById('nets'),st=document.getElementById('scanState');
  st.textContent='SCANNING…';
  n.textContent='Scanning nearby 2.4 GHz networks…';
  try {
    let r=await fetch('/wifi/scan', {
      cache:'no-store'
    });
    if(!r.ok)throw Error('HTTP '+r.status);
    networks=await r.json();
    networks.sort((a,b)=>b.rssi-a.rssi);
    renderNetworks();
    st.textContent='FOUND '+networks.length
  }
  catch(e) {
    st.textContent='SCAN FAILED';
    n.textContent='Scan failed. You can enter the Wi-Fi name manually.'
  }

}

async function refreshStatus() {
  try {
    let r=await fetch('/wifi/status', {
      cache:'no-store'
    });
    if(!r.ok)throw Error();
    let d=await r.json();
    document.getElementById('deviceId').textContent=d.deviceId;
    document.getElementById('deviceMode').textContent=d.connected?'Router connected':'Setup / recovery mode';
    document.getElementById('connectionDetails').textContent='Network: '+d.ssid+' · IP: '+d.ip+(d.connected?' · Signal: '+d.rssi+' dBm':' · Connect your phone to SmartFault-Setup');

  }
  catch(e) {
    document.getElementById('connectionDetails').textContent='Status unavailable; reconnect to the ESP32 setup Wi-Fi and refresh.'
  }

}

document.getElementById('wf').onsubmit=async e=> {
  e.preventDefault();
  let b=e.target.querySelector('button[type=submit]');
  b.disabled=true;
  b.textContent='Saving…';
  let f=new FormData(e.target);
  try {
    let r=await fetch('/wifi/save', {
      method:'POST',headers: {
        'Content-Type':'application/x-www-form-urlencoded'
      },body:new URLSearchParams( {
        ssid:f.get('ssid'),pass:f.get('pass')
      })
    });
    document.getElementById('msg').textContent=await r.text()
  }
  catch(e) {
    document.getElementById('msg').textContent='Connection interrupted; wait for restart, then check the router.'
  }
  setTimeout(()=> {
    b.disabled=false;
    b.textContent='Save and connect'
  },6000)
};

async function forgetWifi() {
  if(!confirm('Forget saved Wi-Fi and restart into setup mode?'))return;
  try {
    let r=await fetch('/wifi/forget', {
      method:'POST'
    });
    document.getElementById('msg').textContent=await r.text()
  }
  catch(e) {
    document.getElementById('msg').textContent='Request failed or restart started. Reconnect to the ESP32 setup Wi-Fi.'
  }

}

scan();
refreshStatus();
setInterval(refreshStatus,10000);
  </script>
 </body>
</html>

)HTML";
  html.replace("SmartFault-Setup", setupSSID);
  return html;
}

void handleWifiStatus() {
  String ssid = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : setupSSID;
  String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : WiFi.softAPIP().toString();
  String j = "{";
  j += "\"connected\":" + String(WiFi.status() == WL_CONNECTED ? "true" : "false");
  j += ",\"setupAP\":" + String(WiFi.softAPIP().toString() != "0.0.0.0" ? "true" : "false");
  j += ",\"ssid\":\"" + jsonEscape(ssid) + "\"";
  j += ",\"ip\":\"" + jsonEscape(ip) + "\"";
  j += ",\"rssi\":" + String(WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0);
  j += ",\"deviceId\":\"" + jsonEscape(deviceId) + "\"";
  j += ",\"hostname\":\"" + jsonEscape(deviceHost + ".local") + "\"}";
  server.send(200, "application/json; charset=utf-8", j);
}

void handleWifiScan() {
  int n = WiFi.scanNetworks(false, true);
  String json = "[";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String name = WiFi.SSID(i);
    if (name.length() == 0) continue;
    if (!first) json += ",";
    first = false;
    json += "{\"ssid\":\"" + jsonEscape(name) + "\",\"rssi\":" + String(WiFi.RSSI(i)) + ",\"channel\":" + String(WiFi.channel(i)) + ",\"secure\":" + String(WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "false" : "true") + "}";
  }
  json += "]";
  WiFi.scanDelete();
  server.send(200, "application/json; charset=utf-8", json);
}

void handleWifiSave() {
  // Initial provisioning is open only on the password-protected setup AP.
  // Changing credentials from the router network requires admin authentication.
  if (!setupPortalMode && !requireAdmin()) return;
  if (!server.hasArg("ssid")) { server.send(400, "text/plain", "Missing Wi-Fi name"); return; }
  String ssid = server.arg("ssid");
  String pass = server.arg("pass");
  ssid.trim();
  if (ssid.length() < 1 || ssid.length() > 32 || pass.length() > 63) {
    server.send(400, "text/plain", "Invalid SSID or password length"); return;
  }
  prefs.begin("wifi", false);
  prefs.putString("ssid", ssid);
  prefs.putString("pass", pass);
  prefs.end();
  server.send(200, "text/plain; charset=utf-8", "Wi-Fi saved. ESP32 is restarting now. If connection fails, SmartFault-Setup will return.");
  delay(700);
  ESP.restart();
}

void handleWifiForget() {
  if (!setupPortalMode && !requireAdmin()) return;
  prefs.begin("wifi", false);
  prefs.remove("ssid"); prefs.remove("pass");
  prefs.end();
  server.send(200, "text/plain", "Saved Wi-Fi forgotten. Restarting into setup mode.");
  delay(500);
  ESP.restart();
}

void setup() {
  bootMs = millis();
  Serial.begin(115200);
  uint64_t mac = ESP.getEfuseMac();
  char idBuf[9]; snprintf(idBuf, sizeof(idBuf), "%08X", (uint32_t)(mac & 0xFFFFFFFF));
  deviceId = String(idBuf);
  deviceHost = String("smartfault-") + String(idBuf + 4); // short per-board hostname
  setupSSID = String(AP_SSID_BASE) + String(idBuf + 4); // unique setup SSID per ESP32
  loadPersistentHistory();
  pinMode(PIN_GREEN, OUTPUT); pinMode(PIN_YELLOW, OUTPUT); pinMode(PIN_RED, OUTPUT);
  pinMode(PIN_BUZZER, OUTPUT);
  pinMode(PIN_RELAY, OUTPUT);
  digitalWrite(PIN_GREEN, LOW); digitalWrite(PIN_YELLOW, LOW); digitalWrite(PIN_RED, LOW);
  digitalWrite(PIN_BUZZER, LOW);
  setRelayOff();
  analogReadResolution(12);
  analogSetPinAttenuation(PIN_CURRENT, ADC_11db);
  analogSetPinAttenuation(PIN_VIN, ADC_11db);
  analogSetPinAttenuation(PIN_VOUT, ADC_11db);
  Wire.begin(21, 22);
  lcd.init(); lcd.backlight(); lcd.clear(); lcd.setCursor(0,0); lcd.print("Smart IoT Fault V6.1"); lcd.setCursor(0,1); lcd.print("Starting...");

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

  // Try saved router credentials first. Keep setup AP active while connecting,
  // so a failed connection always has a recovery path.
  prefs.begin("wifi", true);
  String savedSSID = prefs.getString("ssid", "");
  String savedPass = prefs.getString("pass", "");
  prefs.end();
  WiFi.persistent(false);
  WiFi.setAutoReconnect(true);
  WiFi.setHostname(deviceHost.c_str());
  WiFi.mode(WIFI_AP_STA);
  bool apStarted = WiFi.softAP(setupSSID.c_str(), AP_PASSWORD);
  Serial.println(); Serial.println("SmartFault Wi-Fi manager");
  if (apStarted) { Serial.print("Setup Wi-Fi: "); Serial.print(setupSSID); Serial.print(" | Password: "); Serial.print(AP_PASSWORD); Serial.println(" | IP: 192.168.4.1"); }
  if (savedSSID.length()) {
    Serial.print("Trying saved network: "); Serial.println(savedSSID);
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
    unsigned long startTry = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - startTry < 18000) { delay(250); }
  }
  if (WiFi.status() == WL_CONNECTED) {
    setupPortalMode = false;
    connectedSSID = WiFi.SSID();
    startDeviceMdns();
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    Serial.print("Connected Wi-Fi: "); Serial.println(connectedSSID);
    Serial.print("Dashboard IP: http://"); Serial.println(WiFi.localIP());
    addEvent("Connected to saved Wi-Fi; dashboard at " + WiFi.localIP().toString());
  } else {
    setupPortalMode = true;
    WiFi.mode(WIFI_AP_STA); // STA interface enabled for nearby-network scanning
    if (!apStarted) WiFi.softAP(setupSSID.c_str(), AP_PASSWORD);
    Serial.print("Setup portal IP: "); Serial.println(WiFi.softAPIP());
    addEvent("Wi-Fi setup portal ready at " + WiFi.softAPIP().toString());
  }

  server.on("/", HTTP_GET, [](){
    if (setupPortalMode) server.send(200, "text/html; charset=utf-8", wifiManagerHtml());
    else server.send(200, "text/html; charset=utf-8", dashboardHtml());
  });
  server.on("/wifi", HTTP_GET, [](){ server.send(200, "text/html; charset=utf-8", wifiManagerHtml()); });
  server.on("/wifi/scan", HTTP_GET, handleWifiScan);
  server.on("/wifi/status", HTTP_GET, handleWifiStatus);
  server.on("/wifi/save", HTTP_POST, handleWifiSave);
  server.on("/wifi/forget", HTTP_POST, handleWifiForget);
  server.on("/dashboard", HTTP_GET, [](){ server.send(200, "text/html; charset=utf-8", dashboardHtml()); });
  server.on("/api", HTTP_GET, sendApi);
  server.on("/relay", HTTP_POST, handleRelayControl);
  server.on("/admin/settings", HTTP_GET, sendAdminSettings);
  server.on("/admin/settings", HTTP_POST, handleAdminSave);
  server.on("/admin/test", HTTP_POST, handleAdminTest);
  server.on("/admin/energy-reset", HTTP_POST, [](){ if(!requireAdmin()) return; energyWh=0; addEvent("Energy counter reset"); server.send(200,"application/json","{\"ok\":true}"); });
  server.on("/admin/restart", HTTP_POST, [](){ if(!requireAdmin()) return; server.send(200,"text/plain","Restarting"); delay(250); ESP.restart(); });
  server.on("/admin/events.csv", HTTP_GET, handleAdminEventsCsv);
  server.onNotFound([](){ server.send(404, "text/plain", "Not found"); });
  server.begin();
  if (WiFi.status() == WL_CONNECTED) startDeviceMdns();
}

void loop() {
  server.handleClient();

  // Network resilience: retry router association, and restore the setup AP if the
  // router stays unavailable. Never reboot-loop solely because Wi-Fi is down.
  if (!setupPortalMode && WiFi.status() != WL_CONNECTED) {
    if (wifiDisconnectedSince == 0) { wifiDisconnectedSince = millis(); addEvent("Router Wi-Fi disconnected"); }
    if (millis() - lastReconnectTry >= 10000) {
      lastReconnectTry = millis();
      WiFi.reconnect();
    }
    if (millis() - wifiDisconnectedSince >= 30000) {
      setupPortalMode = true;
      WiFi.mode(WIFI_AP_STA);
      WiFi.softAP(setupSSID.c_str(), AP_PASSWORD);
      addEvent("Router unavailable; setup portal restored");
    }
  } else if (WiFi.status() == WL_CONNECTED) {
    if (wifiDisconnectedSince != 0) addEvent("Router Wi-Fi reconnected");
    wifiDisconnectedSince = 0;
    // Handle a router that connects after the initial startup timeout as well.
    if (connectedSSID.length() == 0) connectedSSID = WiFi.SSID();
    // Once the saved router is actually connected, retire the temporary setup AP.
    if (setupPortalMode && connectedSSID.length() > 0) {
      setupPortalMode = false;
      WiFi.softAPdisconnect(true);
      WiFi.mode(WIFI_STA);
      addEvent("Router connected; setup AP disabled");
    }
    startDeviceMdns();
  }

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
