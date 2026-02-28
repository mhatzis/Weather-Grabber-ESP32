#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <Update.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// ────────────────────────────────────────────────
//  Pin & Constants
// ────────────────────────────────────────────────
#define RELAY_PIN           16
#define RELAY_ACTIVE_LOW    true

#define RESET_BUTTON_PIN    0      // GPIO 0 – long press to reset
#define LED_FEEDBACK_PIN    23     // GPIO 23 – LED for visual feedback
#define LONG_PRESS_MS       10000  // 10 seconds
#define BLINK_START_MS      5000   // start blinking after 5 seconds
#define BLINK_INTERVAL_MS   150    // blink every 150 ms

#define TASK_WEATHER_PRIO   1
#define TASK_WEATHER_STACK  8192
#define TASK_WEB_PRIO       2
#define TASK_WEB_STACK      8192

// Globals
Preferences prefs;
HTTPClient http;
WebServer server(80);

const char* www_username = "admin";
const char* www_password = "admin";   // default – will be loaded from prefs

String owmApiKey   = "";
String owmLat      = "50.8503";
String owmLon      = "4.3517";
bool   owmEnabled  = false;
float  rainThresholdPct = 30.0f;
String apiVersion  = "2.5";
bool   use48hMode  = false;

String mqttBroker  = "broker.hivemq.com";
bool   mqttTlsEnabled = true;
String mqttTlsVersion = "1.3";
String mqttSni     = "";
String mqttUser    = "";
String mqttPass    = "";
String mqttTopic   = "irrigation/control";
bool   mqttEnabled = false;

WiFiClient       plainClient;
WiFiClientSecure secureClient;
PubSubClient     mqttClient(plainClient);

bool   relayInverted = false;
int    manualRelayOverride = 0;   // 0=auto, 1=force ON, -1=force OFF

String lastWeatherStatus = "No check performed yet";
String locationName      = "Unknown location";

unsigned long weatherCheckInterval = 3600000UL;

bool lastHighRainRisk = false;
int  lastMaxPop       = 0;
int  lastHighCount    = 0;

// Reset button state
unsigned long buttonPressStart = 0;
bool buttonWasPressed = false;
unsigned long lastBlinkTime = 0;
bool ledState = false;

// ────────────────────────────────────────────────
//  Shared CSS
// ────────────────────────────────────────────────
const char* sharedCSS = R"rawliteral(
<!DOCTYPE html>
<html lang="en">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <link href="https://fonts.googleapis.com/css2?family=Roboto:wght@400;500;700&display=swap" rel="stylesheet">
  <style>
    :root {
      --primary: #6750a4;
      --surface: #fef7ff;
      --on-surface: #1c1b1f;
      --outline: #79747e;
      --container: #eaddff;
      --shadow: 0 1px 3px rgba(0,0,0,0.12);
      --switch-bg: #e0e0e0;
      --switch-active: var(--primary);
      --tab-bg: #f5f5f5;
      --tab-active: var(--primary);
    }
    body { margin:0; font-family:'Roboto',sans-serif; background:var(--surface); color:var(--on-surface); min-height:100vh; padding:16px; box-sizing:border-box; }
    .container { max-width:420px; margin:0 auto; }
    .card { background:white; border-radius:28px; padding:24px; margin-bottom:24px; box-shadow:var(--shadow); }
    h1 { font-size:28px; margin:0 0 24px; color:var(--primary); }
    .status { padding:16px; border-radius:16px; background:var(--container); color:var(--primary); text-align:center; margin-bottom:24px; }
    .field { margin-bottom:24px; }
    label { display:block; margin-bottom:8px; font-weight:500; }
    input, select, textarea { width:100%; padding:12px 16px; border:1px solid var(--outline); border-radius:12px; font-size:16px; box-sizing:border-box; }
    textarea[readonly] { background:#f5f5f5; resize:none; height:100px; }
    .switch { position:relative; display:inline-block; width:54px; height:28px; }
    .switch input { opacity:0; width:0; height:0; }
    .slider { position:absolute; cursor:pointer; top:0; left:0; right:0; bottom:0; background:var(--switch-bg); transition:.4s; border-radius:28px; }
    .slider:before { position:absolute; content:""; height:24px; width:24px; left:2px; bottom:2px; background:white; transition:.4s; border-radius:50%; }
    input:checked + .slider { background-color:var(--switch-active); }
    input:checked + .slider:before { transform:translateX(26px); }
    .tabs { display:flex; margin-bottom:20px; border-bottom:2px solid var(--outline); overflow-x:auto; }
    .tab { flex:1; text-align:center; padding:12px 16px; background:var(--tab-bg); color:var(--on-surface); cursor:pointer; font-weight:500; border-radius:12px 12px 0 0; transition:all 0.3s; min-width:100px; }
    .tab.active { background:var(--tab-active); color:white; border-bottom:3px solid var(--tab-active); }
    .tab-content { display:none; }
    .tab-content.active { display:block; }
    button { background:var(--primary); color:white; border:none; border-radius:20px; padding:12px 24px; font-size:16px; font-weight:500; cursor:pointer; width:100%; margin:8px 0; }
    button:hover { opacity:0.92; }
    a { color:var(--primary); text-decoration:none; }
    .back { margin-top:24px; text-align:center; }
    .disclaimer { text-align:center; color:#666; font-size:0.9em; margin-top:32px; padding:16px; border-top:1px solid #ddd; }
    #status, #location, #lastpop { font-weight:bold; color:var(--primary); }
  </style>
)rawliteral";

// Forward declarations
void performWeatherCheck();
void TaskWeather(void *pvParameters);
void TaskWebServer(void *pvParameters);
void handleRoot();
void handleSettings();
void handleSave();
void handleUpdate();
void handleDoUpdate();
void handleTestOWM();
void handleManualCheck();
void connectMQTT();
void switchMqttClient(bool useTls);
void publishWeatherStats();
void mqttCallback(char* topic, byte* payload, unsigned int length);
void updateRelayState();
void checkResetButton();

// ────────────────────────────────────────────────
//  Setup
// ────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println("\n=== Irrigation Controller starting ===");

  pinMode(RELAY_PIN, OUTPUT);
  digitalWrite(RELAY_PIN, RELAY_ACTIVE_LOW ? HIGH : LOW);
  Serial.println("[Boot] Relay forced OFF at startup");

  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  pinMode(LED_FEEDBACK_PIN, OUTPUT);
  digitalWrite(LED_FEEDBACK_PIN, LOW);  // LED off at boot

  // Load admin credentials from preferences (default admin/admin)
  prefs.begin("web_auth", false);
  String storedUser = prefs.getString("username", "admin");
  String storedPass = prefs.getString("password", "admin");
  prefs.end();

  www_username = strdup(storedUser.c_str());  // Convert String to const char*
  www_password = strdup(storedPass.c_str());

  prefs.begin("weather", false);
  owmApiKey       = prefs.getString("apikey", "");
  owmLat          = prefs.getString("lat", "50.8503");
  owmLon          = prefs.getString("lon", "4.3517");
  owmEnabled      = prefs.getBool("enabled", false);
  rainThresholdPct = prefs.getFloat("rainpct", 30.0f);
  apiVersion      = prefs.getString("apiver", "2.5");
  use48hMode      = prefs.getBool("use48h", false);
  mqttBroker      = prefs.getString("mqttbroker", "broker.hivemq.com");
  mqttTlsEnabled  = prefs.getBool("mqtttls", true);
  mqttTlsVersion  = prefs.getString("mtlsver", "1.3");
  mqttSni         = prefs.getString("mqttsni", "");
  mqttUser        = prefs.getString("mqttuser", "");
  mqttPass        = prefs.getString("mqttpass", "");
  mqttTopic       = prefs.getString("mqtttopic", "irrigation/control");
  mqttEnabled     = prefs.getBool("mqttenabled", false);
  relayInverted   = prefs.getBool("relayinvert", false);
  manualRelayOverride = prefs.getInt("relayoverride", 0);
  weatherCheckInterval = prefs.getULong("checkinterval", 3600000UL);
  prefs.end();

  Serial.printf("Loaded: MQTT enabled=%d | TLS=%d | RelayOverride=%d\n", 
                mqttEnabled, mqttTlsEnabled, manualRelayOverride);

  WiFiManager wm;
  wm.setDebugOutput(true);
  wm.setSaveConfigCallback([]() { Serial.println("WiFi config saved"); });

  if (!wm.autoConnect("Irrigation-AP", "password123")) {
    Serial.println("Failed to connect → restart");
    delay(3000);
    ESP.restart();
  }

  Serial.print("Connected! IP: ");
  Serial.println(WiFi.localIP());

  IPAddress dns(8, 8, 8, 8);
  WiFi.config(WiFi.localIP(), WiFi.gatewayIP(), WiFi.subnetMask(), dns);
  Serial.println("[WiFi] Using Google DNS 8.8.8.8");

  WiFi.setSleep(false);

  mqttClient.setCallback(mqttCallback);
  mqttClient.setSocketTimeout(30);
  mqttClient.setKeepAlive(60);

  if (mqttEnabled) {
    switchMqttClient(mqttTlsEnabled);
    connectMQTT();
  }

  updateRelayState();

  performWeatherCheck();
  Serial.println("[Boot] Initial weather check completed");

  xTaskCreate(TaskWebServer, "WebServer", TASK_WEB_STACK, NULL, TASK_WEB_PRIO, NULL);
  xTaskCreate(TaskWeather,   "Weather",   TASK_WEATHER_STACK, NULL, TASK_WEATHER_PRIO, NULL);
}

// ────────────────────────────────────────────────
//  Reset button check with LED feedback
// ────────────────────────────────────────────────
void checkResetButton() {
  bool buttonState = digitalRead(RESET_BUTTON_PIN) == LOW;  // active LOW

  if (buttonState) {
    if (!buttonWasPressed) {
      buttonPressStart = millis();
      buttonWasPressed = true;
      digitalWrite(LED_FEEDBACK_PIN, HIGH);
      lastBlinkTime = millis();
      Serial.println("[Reset] Button pressed – hold 10s to reset WiFi + admin credentials (LED ON)");
    }

    unsigned long heldMs = millis() - buttonPressStart;

    // Blink after 5 seconds
    if (heldMs >= BLINK_START_MS) {
      if (millis() - lastBlinkTime >= BLINK_INTERVAL_MS) {
        ledState = !ledState;
        digitalWrite(LED_FEEDBACK_PIN, ledState ? HIGH : LOW);
        lastBlinkTime = millis();
      }
    }

    // Long press complete
    if (heldMs >= LONG_PRESS_MS) {
      Serial.println("[Reset] 10s long press detected → resetting");

      digitalWrite(LED_FEEDBACK_PIN, LOW);  // LED off

      // Reset WiFi
      WiFiManager wm;
      wm.resetSettings();
      Serial.println("[Reset] WiFi credentials erased");

      // Reset admin credentials
      prefs.begin("web_auth", false);
      prefs.putString("username", "admin");
      prefs.putString("password", "admin");
      prefs.end();
      Serial.println("[Reset] Admin reset to admin/admin");

      // Confirmation blinks
      for (int i = 0; i < 6; i++) {
        digitalWrite(LED_FEEDBACK_PIN, HIGH);
        delay(150);
        digitalWrite(LED_FEEDBACK_PIN, LOW);
        delay(150);
      }

      Serial.println("[Reset] Rebooting...");
      delay(500);
      ESP.restart();
    }
  } else {
    if (buttonWasPressed) {
      digitalWrite(LED_FEEDBACK_PIN, LOW);
      buttonWasPressed = false;
      Serial.println("[Reset] Button released early – no action");
    }
  }
}

// ────────────────────────────────────────────────
//  Switch MQTT client
// ────────────────────────────────────────────────
void switchMqttClient(bool useTls) {
  if (useTls) {
    mqttClient.setClient(secureClient);
    secureClient.setInsecure();
    mqttClient.setServer(mqttBroker.c_str(), 8883);
    Serial.println("[MQTT] Using TLS on 8883");
  } else {
    mqttClient.setClient(plainClient);
    mqttClient.setServer(mqttBroker.c_str(), 1883);
    Serial.println("[MQTT] Using plain MQTT on 1883");
  }
}

// ────────────────────────────────────────────────
//  MQTT connect / reconnect
// ────────────────────────────────────────────────
void connectMQTT() {
  if (!mqttEnabled) return;

  if (mqttClient.connected()) return;

  Serial.println("┌──────────────────────────────────────────────┐");
  Serial.print  ("│ [MQTT] Connecting to:                        │ ");
  Serial.print(mqttBroker);
  Serial.println(" │");
  Serial.print  ("│ Port:                                        │ ");
  Serial.print(mqttTlsEnabled ? 8883 : 1883);
  Serial.println(" (TLS: " + String(mqttTlsEnabled ? "YES" : "NO") + ") │");
  Serial.print  ("│ Username:                                    │ ");
  Serial.println(mqttUser.length() > 0 ? mqttUser : "<empty>");
  Serial.print  ("│ Password length:                             │ ");
  Serial.print(mqttPass.length());
  Serial.println(" chars" + String(mqttPass.length() > 0 ? " (present)" : " (empty)") + " │");
  Serial.println("└──────────────────────────────────────────────┘");

  String clientId = "IrrigationESP32-" + String(random(0xffff), HEX);

  bool connected = false;
  if (mqttUser.length() > 0 && mqttPass.length() > 0) {
    connected = mqttClient.connect(clientId.c_str(), mqttUser.c_str(), mqttPass.c_str());
  } else {
    connected = mqttClient.connect(clientId.c_str());
  }

  if (connected) {
    Serial.println("[MQTT] Connected!");

    String rainCmd = mqttTopic + "/command/enable_rain";
    mqttClient.subscribe(rainCmd.c_str());
    Serial.print("[MQTT] Subscribed to: ");
    Serial.println(rainCmd);

    String relayCmd = mqttTopic + "/command/relay";
    mqttClient.subscribe(relayCmd.c_str());
    Serial.print("[MQTT] Subscribed to: ");
    Serial.println(relayCmd);
  } else {
    Serial.print("[MQTT] Failed, rc=");
    Serial.print(mqttClient.state());
    Serial.println(" → retry in 5s");
    delay(5000);
  }
}

// ────────────────────────────────────────────────
//  MQTT callback (rain enable & relay commands)
// ────────────────────────────────────────────────
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  String topicStr = String(topic);
  String payloadStr = "";

  for (unsigned int i = 0; i < length; i++) {
    payloadStr += (char)payload[i];
  }
  payloadStr.trim();
  payloadStr.toLowerCase();

  Serial.print("[MQTT] Message on ");
  Serial.print(topicStr);
  Serial.print(" → ");
  Serial.println(payloadStr);

  String rainCmdTopic  = mqttTopic + "/command/enable_rain";
  String relayCmdTopic = mqttTopic + "/command/relay";

  if (topicStr == rainCmdTopic) {
    bool newEnabled = false;
    if (payloadStr == "on" || payloadStr == "1" || payloadStr == "true" || payloadStr == "enable") {
      newEnabled = true;
    } else if (payloadStr == "off" || payloadStr == "0" || payloadStr == "false" || payloadStr == "disable") {
      newEnabled = false;
    } else {
      Serial.println("[MQTT] Invalid enable_rain command");
      return;
    }

    if (newEnabled != owmEnabled) {
      owmEnabled = newEnabled;
      prefs.begin("weather");
      prefs.putBool("enabled", owmEnabled);
      prefs.end();

      Serial.printf("[MQTT] Rain forecast %s via MQTT\n", owmEnabled ? "ENABLED" : "DISABLED");
      performWeatherCheck();

      String stateTopic = mqttTopic + "/status/rain_enabled";
      mqttClient.publish(stateTopic.c_str(), owmEnabled ? "true" : "false");
    }
  }
  else if (topicStr == relayCmdTopic) {
    int newOverride = 0;
    if (payloadStr == "on" || payloadStr == "1" || payloadStr == "true" || payloadStr == "enable") {
      newOverride = 1;
    } else if (payloadStr == "off" || payloadStr == "0" || payloadStr == "false" || payloadStr == "disable") {
      newOverride = -1;
    } else if (payloadStr == "auto" || payloadStr == "weather") {
      newOverride = 0;
    } else {
      Serial.println("[MQTT] Invalid relay command");
      return;
    }

    if (newOverride != manualRelayOverride) {
      manualRelayOverride = newOverride;
      prefs.begin("weather");
      prefs.putInt("relayoverride", manualRelayOverride);
      prefs.end();

      Serial.printf("[MQTT] Relay override set to %d (%s)\n", 
                    manualRelayOverride,
                    manualRelayOverride == 1 ? "ON" : 
                    manualRelayOverride == -1 ? "OFF" : "auto");

      updateRelayState();

      String relayState = (manualRelayOverride == 1) ? "on" : 
                          (manualRelayOverride == -1) ? "off" : "auto";
      mqttClient.publish((mqttTopic + "/status/relay").c_str(), relayState.c_str());
    }
  }
}

// ────────────────────────────────────────────────
//  Update relay state
// ────────────────────────────────────────────────
void updateRelayState() {
  bool shouldWater;

  if (manualRelayOverride == 1) {
    shouldWater = true;
    Serial.println("[Relay] Forced ON (manual override)");
  } else if (manualRelayOverride == -1) {
    shouldWater = false;
    Serial.println("[Relay] Forced OFF (manual override)");
  } else {
    shouldWater = !lastHighRainRisk;
    Serial.println("[Relay] Auto mode – weather decision");
  }

  if (relayInverted) shouldWater = !shouldWater;

  int gpioLevel = shouldWater ? HIGH : LOW;
  if (RELAY_ACTIVE_LOW) gpioLevel = !gpioLevel;

  digitalWrite(RELAY_PIN, gpioLevel);

  if (mqttEnabled && mqttClient.connected()) {
    String state = shouldWater ? "on" : "off";
    mqttClient.publish((mqttTopic + "/status/relay").c_str(), state.c_str());
  }
}

// ────────────────────────────────────────────────
//  Weather check
// ────────────────────────────────────────────────
void performWeatherCheck() {
  if (WiFi.status() != WL_CONNECTED || !owmEnabled || owmApiKey.length() < 10) {
    lastWeatherStatus = owmEnabled ? "No WiFi connection" : "OpenWeatherMap disabled";
    Serial.println("[Weather] " + lastWeatherStatus);
    return;
  }

  int hours = use48hMode ? 48 : 24;
  int minHighSlots = use48hMode ? 3 : 2;

  Serial.printf("[Weather] %dh check using API %s (threshold = %.1f%%, min high slots = %d)\n",
                hours, apiVersion.c_str(), rainThresholdPct, minHighSlots);

  String url;
  bool highRainRisk = false;
  String statusMsg = "Check failed";

  if (apiVersion == "3.0") {
    url = "https://api.openweathermap.org/data/3.0/onecall?lat=" + owmLat +
          "&lon=" + owmLon + "&appid=" + owmApiKey + "&units=metric&exclude=current,minutely,alerts";
  } else {
    url = "http://api.openweathermap.org/data/2.5/forecast?lat=" + owmLat +
          "&lon=" + owmLon + "&appid=" + owmApiKey + "&units=metric&cnt=" + String(hours <= 24 ? 8 : 16);
  }

  http.begin(url);
  int code = http.GET();

  if (code == HTTP_CODE_OK) {
    String payload = http.getString();

    int highCount = 0;
    int maxPop = 0;
    int popCount = 0;

    int pos = 0;
    while ((pos = payload.indexOf("\"pop\":", pos)) != -1) {
      pos += 6;
      int end = payload.indexOf(",", pos);
      if (end == -1) end = payload.indexOf("}", pos);
      if (end == -1) break;

      String val = payload.substring(pos, end);
      val.trim();

      float popFloat = val.toFloat();
      int popPercent = (int)(popFloat * 100.0f);

      if (popPercent > maxPop) maxPop = popPercent;

      if (popPercent >= (int)rainThresholdPct) {
        highCount++;
      }

      popCount++;
      pos = end;
    }

    lastMaxPop = maxPop;
    lastHighCount = highCount;

    statusMsg = "Max rain prob (next " + String(hours) + "h): " + String(maxPop) + "% | " +
                String(highCount) + " slots ≥ " + String((int)rainThresholdPct) + "%";

    if (highCount >= minHighSlots) {
      highRainRisk = true;
      statusMsg += " → OFF (≥" + String(minHighSlots) + " high-risk slots)";
    } else {
      statusMsg += " → ON (only " + String(highCount) + " high-risk slot(s))";
    }
  } else {
    statusMsg += " (HTTP " + String(code) + ")";
  }

  http.end();

  lastHighRainRisk = highRainRisk;

  bool finalDecision;
  if (manualRelayOverride == 1) {
    finalDecision = true;
  } else if (manualRelayOverride == -1) {
    finalDecision = false;
  } else {
    finalDecision = !lastHighRainRisk;
  }

  bool finalState = finalDecision;
  if (relayInverted) finalState = !finalState;

  int gpioLevel = finalState ? HIGH : LOW;
  if (RELAY_ACTIVE_LOW) gpioLevel = !gpioLevel;

  digitalWrite(RELAY_PIN, gpioLevel);

  lastWeatherStatus = statusMsg;

  Serial.println("[Weather] " + lastWeatherStatus);

  publishWeatherStats();
  updateRelayState();
}

// ────────────────────────────────────────────────
//  Publish weather stats
// ────────────────────────────────────────────────
void publishWeatherStats() {
  if (!mqttEnabled || !mqttClient.connected()) {
    Serial.println("[MQTT Publish] Skipped – MQTT disabled or not connected");
    return;
  }

  String base = mqttTopic + "/";

  String status = lastHighRainRisk ? "Watering Paused" : "Watering Allowed";
  mqttClient.publish((base + "status").c_str(), status.c_str());

  String risk = lastHighRainRisk ? "high" : "low";
  mqttClient.publish((base + "rain_risk").c_str(), risk.c_str());

  mqttClient.publish((base + "last_message").c_str(), lastWeatherStatus.c_str());

  mqttClient.publish((base + "max_pop").c_str(), String(lastMaxPop).c_str());
  mqttClient.publish((base + "high_slots").c_str(), String(lastHighCount).c_str());
  mqttClient.publish((base + "threshold").c_str(), String((int)rainThresholdPct).c_str());

  String hoursStr = use48hMode ? "48" : "24";
  mqttClient.publish((base + "forecast_hours").c_str(), hoursStr.c_str());

  String invertStr = relayInverted ? "true" : "false";
  mqttClient.publish((base + "relay_inverted").c_str(), invertStr.c_str());

  unsigned long uptime = millis() / 1000;
  mqttClient.publish((base + "uptime_sec").c_str(), String(uptime).c_str());

  Serial.println("[MQTT] Published all weather & status stats");
}

// ────────────────────────────────────────────────
//  Weather task
// ────────────────────────────────────────────────
void TaskWeather(void *pvParameters) {
  for (;;) {
    performWeatherCheck();

    if (!owmEnabled || WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(10000));
    } else {
      vTaskDelay(weatherCheckInterval);
    }
  }
}

// ────────────────────────────────────────────────
//  Web server task
// ────────────────────────────────────────────────
void TaskWebServer(void *pvParameters) {
  server.on("/", handleRoot);
  server.on("/settings", HTTP_GET, handleSettings);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/update", HTTP_GET, handleUpdate);
  server.on("/update", HTTP_POST, []() {
    server.sendHeader("Connection", "close");
    server.send(200, "text/plain", (Update.hasError()) ? "FAIL" : "OK");
    if (!Update.hasError()) {
      Serial.println("[OTA] Success → rebooting");
      delay(1000);
      ESP.restart();
    }
  }, handleDoUpdate);

  server.on("/testowm", HTTP_GET, handleTestOWM);
  server.on("/check", HTTP_GET, handleManualCheck);

  server.begin();
  Serial.println("[Web] Server task started");

  for (;;) {
    server.handleClient();
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ────────────────────────────────────────────────
//  Main page
// ────────────────────────────────────────────────
void handleRoot() {
  if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();

  String relayStatus = lastHighRainRisk ? "Watering paused" : "Watering allowed";

  String html = String(sharedCSS) + R"rawliteral(
  <title>Irrigation Control</title>
</head>
<body>
  <div class="container">
    <div class="card">
      <h1>Irrigation Control</h1>
      <div class="status">
        Status: <strong>)rawliteral" + relayStatus + R"rawliteral(</strong><br>
        IP: )rawliteral" + WiFi.localIP().toString() + R"rawliteral(
      </div>
      <p style="text-align:center; margin:16px 0;">
        <a href="/settings"><button>Settings</button></a>
      </p>
    </div>
  </div>
</body>
</html>
  )rawliteral";

  server.send(200, "text/html", html);
}

// ────────────────────────────────────────────────
//  Settings page
// ────────────────────────────────────────────────
void handleSettings() {
  if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();

  prefs.begin("weather");
  String apikey = prefs.getString("apikey", "");
  String lat    = prefs.getString("lat", "50.8503");
  String lon    = prefs.getString("lon", "4.3517");
  bool enabled  = prefs.getBool("enabled", false);
  float rainpct = prefs.getFloat("rainpct", 30.0);
  String apiVer = prefs.getString("apiver", "2.5");
  bool use48h   = prefs.getBool("use48h", false);
  String mqttBrokerSaved = prefs.getString("mqttbroker", "broker.hivemq.com");
  bool mqttTlsSaved = prefs.getBool("mqtttls", true);
  String mqttTlsVerSaved = prefs.getString("mtlsver", "1.3");
  String mqttSniSaved = prefs.getString("mqttsni", "");
  String mqttUserSaved   = prefs.getString("mqttuser", "");
  String mqttPassSaved   = prefs.getString("mqttpass", "");
  String mqttTopicSaved  = prefs.getString("mqtttopic", "irrigation/control");
  bool mqttEnabledSaved  = prefs.getBool("mqttenabled", false);
  bool relayInvertedSaved = prefs.getBool("relayinvert", false);
  int relayOverrideSaved = prefs.getInt("relayoverride", 0);
  unsigned long checkIntervalSaved = prefs.getULong("checkinterval", 3600000UL);
  prefs.end();

  String html = String(sharedCSS) + R"rawliteral(
  <title>Settings</title>
</head>
<body>
  <div class="container">
    <div class="card">
      <h1>Settings</h1>

      <div class="tabs">
        <input type="radio" name="settings-tab" id="tab-owm" checked>
        <label class="tab active" for="tab-owm">OpenWeather</label>

        <input type="radio" name="settings-tab" id="tab-login">
        <label class="tab" for="tab-login">Web Login</label>

        <input type="radio" name="settings-tab" id="tab-mqtt">
        <label class="tab" for="tab-mqtt">MQTT</label>

        <input type="radio" name="settings-tab" id="tab-admin">
        <label class="tab" for="tab-admin">Admin</label>
      </div>

      <form method="POST" action="/save">

        <!-- OpenWeather Tab -->
        <div class="tab-content active" id="content-owm">
          <div class="field">
            <label>OpenWeatherMap API Key</label>
            <input type="text" name="apikey" value=")rawliteral" + apikey + R"rawliteral(" required>
          </div>
          <div class="field">
            <label>Latitude</label>
            <input type="text" name="lat" value=")rawliteral" + lat + R"rawliteral(" required>
          </div>
          <div class="field">
            <label>Longitude</label>
            <input type="text" name="lon" value=")rawliteral" + lon + R"rawliteral(" required>
          </div>

          <div class="field">
            <label>Enable rain forecast check</label>
            <label class="switch">
              <input type="checkbox" name="enabled" value="1" )rawliteral" + (enabled ? "checked" : "") + R"rawliteral(>
              <span class="slider"></span>
            </label>
          </div>

          <div class="field">
            <label>Use One Call API 3.0 (instead of 2.5)</label>
            <label class="switch">
              <input type="checkbox" name="apiver" value="3.0" )rawliteral" + (apiVer == "3.0" ? "checked" : "") + R"rawliteral(>
              <span class="slider"></span>
            </label>
            <small>ON = 3.0 (your subscription), OFF = 2.5 (free legacy)</small>
          </div>

          <div class="field">
            <label>Rain probability threshold (%)</label>
            <input type="number" name="rainpct" min="0" max="100" step="1" value=")rawliteral" + String(rainpct, 1) + R"rawliteral(" required>
            <small>Relay turns OFF if ≥ 2 slots (24h) or ≥ 3 slots (48h) ≥ this value</small>
          </div>

          <div class="field">
            <label>Use 48-hour forecast mode (more conservative)</label>
            <label class="switch">
              <input type="checkbox" name="use48h" value="1" )rawliteral" + (use48h ? "checked" : "") + R"rawliteral(>
              <span class="slider"></span>
            </label>
            <small>OFF = check next 24h (need ≥2 high slots), ON = check next 48h (need ≥3 high slots)</small>
          </div>

          <div class="field">
            <label>Last detected max rain probability (%)</label>
            <input type="text" readonly value=")rawliteral" + lastWeatherStatus + R"rawliteral(">
          </div>

          <button type="button" onclick="testConnection()">Test Connection & Get Location</button>

          <div class="field" style="margin-top:16px;">
            <label>Status</label>
            <input type="text" id="status" readonly value="Not tested yet">
          </div>
          <div class="field">
            <label>Location (city/country)</label>
            <input type="text" id="location" readonly value=")rawliteral" + locationName + R"rawliteral(">
          </div>

          <div class="field">
            <label>Check Frequency</label>
            <select name="checkinterval">
              <option value="900000" )rawliteral" + (checkIntervalSaved == 900000 ? "selected" : "") + R"rawliteral(>15 minutes</option>
              <option value="1800000" )rawliteral" + (checkIntervalSaved == 1800000 ? "selected" : "") + R"rawliteral(>30 minutes</option>
              <option value="3600000" )rawliteral" + (checkIntervalSaved == 3600000 ? "selected" : "") + R"rawliteral(>1 hour</option>
              <option value="7200000" )rawliteral" + (checkIntervalSaved == 7200000 ? "selected" : "") + R"rawliteral(>2 hours</option>
              <option value="14400000" )rawliteral" + (checkIntervalSaved == 14400000 ? "selected" : "") + R"rawliteral(>4 hours</option>
              <option value="21600000" )rawliteral" + (checkIntervalSaved == 21600000 ? "selected" : "") + R"rawliteral(>6 hours</option>
              <option value="43200000" )rawliteral" + (checkIntervalSaved == 43200000 ? "selected" : "") + R"rawliteral(>12 hours</option>
              <option value="86400000" )rawliteral" + (checkIntervalSaved == 86400000 ? "selected" : "") + R"rawliteral(>24 hours</option>
            </select>
          </div>

          <div style="margin:20px 0; display:flex; gap:10px;">
            <button type="button" onclick="manualCheck()" style="flex:1; background:#4caf50;">Check Now</button>
          </div>
        </div>

        <!-- Web Login Tab -->
        <div class="tab-content" id="content-login">
          <div class="field">
            <label>Web Username</label>
            <input type="text" name="webuser" value="admin" required>
          </div>
          <div class="field">
            <label>Web Password</label>
            <input type="password" name="webpass" value="" placeholder="Leave blank to keep current">
          </div>
          <p style="color:#666; font-size:0.9em;">
            Note: Username/password change requires manual code update for security.
          </p>
        </div>

        <!-- MQTT Tab -->
        <div class="tab-content" id="content-mqtt">
          <div class="field">
            <label>Enable MQTT pub/sub</label>
            <label class="switch">
              <input type="checkbox" name="mqttenabled" value="1" )rawliteral" + (mqttEnabledSaved ? "checked" : "") + R"rawliteral(>
              <span class="slider"></span>
            </label>
          </div>
          <div class="field">
            <label>MQTT Broker</label>
            <input type="text" name="mqttbroker" value=")rawliteral" + mqttBrokerSaved + R"rawliteral(" placeholder="e.g. broker.hivemq.com">
          </div>
          <div class="field">
            <label>Use TLS (secure connection – port 8883)</label>
            <label class="switch">
              <input type="checkbox" name="mqtttls" value="1" )rawliteral" + (mqttTlsSaved ? "checked" : "") + R"rawliteral(>
              <span class="slider"></span>
            </label>
          </div>
          <div class="field">
            <label>MQTT Port (auto)</label>
            <input type="text" value=")rawliteral" + (mqttTlsSaved ? "8883 (TLS)" : "1883 (plain)") + R"rawliteral(" readonly>
          </div>
          <div class="field">
            <label>TLS Version (if TLS enabled)</label>
            <select name="mtlsver">
              <option value="1.2" )rawliteral" + (mqttTlsVerSaved == "1.2" ? "selected" : "") + R"rawliteral(>TLS 1.2</option>
              <option value="1.3" )rawliteral" + (mqttTlsVerSaved == "1.3" ? "selected" : "") + R"rawliteral(>TLS 1.3 (recommended)</option>
            </select>
          </div>
          <div class="field">
            <label>SNI (Server Name Indication – optional, usually not needed)</label>
            <input type="text" name="mqttsni" value=")rawliteral" + mqttSniSaved + R"rawliteral(" placeholder="leave blank for most brokers">
          </div>
          <div class="field">
            <label>MQTT Username</label>
            <input type="text" name="mqttuser" value=")rawliteral" + mqttUserSaved + R"rawliteral(">
          </div>
          <div class="field">
            <label>MQTT Password</label>
            <input type="password" name="mqttpass" value="" placeholder="Leave blank to keep current">
          </div>
          <div class="field">
            <label>MQTT Topic</label>
            <input type="text" name="mqtttopic" value=")rawliteral" + mqttTopicSaved + R"rawliteral(" placeholder="e.g. home/irrigation">
          </div>
        </div>

        <!-- Admin Tab -->
        <div class="tab-content" id="content-admin">
          <div class="field">
            <label>Invert Relay Logic</label>
            <label class="switch">
              <input type="checkbox" name="relayinvert" value="1" )rawliteral" + (relayInvertedSaved ? "checked" : "") + R"rawliteral(>
              <span class="slider"></span>
            </label>
            <p style="color:#666; font-size:0.9em; margin-top:8px;">
              When enabled, the relay ON/OFF states are reversed.<br>
              <strong>Changing this will reboot the device automatically.</strong>
            </p>
          </div>

          <div class="field" style="margin-top:24px;">
            <a href="/update"><button type="button" style="background:#d32f2f;">OTA Firmware Update</button></a>
            <p style="color:#666; font-size:0.9em; margin-top:8px;">
              Upload new firmware (.bin file). Do not power off during update.
            </p>
          </div>
        </div>

        <!-- Last weather message -->
        <div class="field">
          <label>Last weather message</label>
          <textarea readonly>)rawliteral" + lastWeatherStatus + R"rawliteral(</textarea>
        </div>

        <button type="submit">Save Settings</button>

        <div class="disclaimer">
          Use at own risk. Created by Labhat group PTY LTD, Opensource projects group
        </div>
      </form>
    </div>
  </div>

  <script>
    document.querySelectorAll('input[name="settings-tab"]').forEach(radio => {
      radio.addEventListener('change', function() {
        document.querySelectorAll('.tab-content').forEach(c => c.classList.remove('active'));
        const contentId = 'content-' + this.id.replace('tab-', '');
        document.getElementById(contentId).classList.add('active');
      });
    });

    document.querySelector('input[name="mqtttls"]').addEventListener('change', function() {
      const portField = document.querySelector('input[type="text"][readonly]');
      if (portField) {
        portField.value = this.checked ? "8883 (TLS)" : "1883 (plain)";
      }
    });

    function testConnection() {
      document.getElementById('status').value = "Testing... please wait";
      fetch('/testowm')
        .then(r => r.json())
        .then(data => {
          document.getElementById('status').value = data.status;
          if (data.location) document.getElementById('location').value = data.location;
        })
        .catch(() => document.getElementById('status').value = "Error contacting server");
    }

    function manualCheck() {
      fetch('/check')
        .then(r => r.text())
        .then(text => alert("Manual check completed:\n" + text))
        .catch(() => alert("Error triggering check"));
    }
  </script>
</body>
</html>
  )rawliteral";

  server.send(200, "text/html", html);
}

// ────────────────────────────────────────────────
//  Save handler
// ────────────────────────────────────────────────
void handleSave() {
  if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();

  String apikey   = server.arg("apikey");
  String lat      = server.arg("lat");
  String lon      = server.arg("lon");
  String enabled  = server.arg("enabled");
  String rainpctStr = server.arg("rainpct");
  String apiver   = server.arg("apiver") == "3.0" ? "3.0" : "2.5";
  String use48hStr = server.arg("use48h");
  String mqttenabledStr = server.arg("mqttenabled");
  String mqttbroker = server.arg("mqttbroker");
  String mqtttlsStr = server.arg("mqtttls");
  String mtlsver    = server.arg("mtlsver");
  String mqttsni    = server.arg("mqttsni");
  String mqttuser   = server.arg("mqttuser");
  String mqttpass   = server.arg("mqttpass");
  String mqtttopic  = server.arg("mqtttopic");
  String relayinvertStr = server.arg("relayinvert");
  String checkintervalStr = server.arg("checkinterval");

  rainpctStr.trim();

  float newThreshold = rainThresholdPct;
  if (rainpctStr.length() > 0) {
    float parsed = rainpctStr.toFloat();
    if (!isnan(parsed) && parsed >= 0 && parsed <= 100) newThreshold = parsed;
  }

  bool newMqttEnabled = (mqttenabledStr == "1");
  bool newMqttTls = (mqtttlsStr == "1");
  String newMqttTlsVer = (mtlsver == "1.2" || mtlsver == "1.3") ? mtlsver : "1.3";
  String newMqttSni = mqttsni;
  bool newRelayInverted = (relayinvertStr == "1");

  unsigned long newCheckInterval = 3600000UL;
  if (checkintervalStr.length() > 0) {
    long val = checkintervalStr.toInt();
    if (val >= 900 && val <= 86400) newCheckInterval = val * 1000UL;
    else Serial.println("[Save] Invalid interval value: " + checkintervalStr);
  }

  prefs.begin("weather");
  prefs.putString("apikey", apikey);
  prefs.putString("lat", lat);
  prefs.putString("lon", lon);
  prefs.putBool("enabled", enabled == "1");
  prefs.putFloat("rainpct", newThreshold);
  prefs.putString("apiver", apiver);
  prefs.putBool("use48h", (use48hStr == "1"));
  prefs.putBool("mqttenabled", newMqttEnabled);
  prefs.putString("mqttbroker", mqttbroker);
  prefs.putBool("mqtttls", newMqttTls);
  prefs.putString("mtlsver", newMqttTlsVer);
  prefs.putString("mqttsni", newMqttSni);
  prefs.putString("mqttuser", mqttuser);
  prefs.putString("mqttpass", mqttpass);
  prefs.putString("mqtttopic", mqtttopic);
  prefs.putBool("relayinvert", newRelayInverted);
  prefs.putULong("checkinterval", newCheckInterval);
  prefs.end();

  owmApiKey = apikey;
  owmLat = lat;
  owmLon = lon;
  owmEnabled = enabled == "1";
  rainThresholdPct = newThreshold;
  apiVersion = apiver;
  use48hMode = (use48hStr == "1");
  mqttEnabled = newMqttEnabled;
  mqttTlsEnabled = newMqttTls;
  mqttTlsVersion = newMqttTlsVer;
  mqttSni = newMqttSni;
  weatherCheckInterval = newCheckInterval;

  if (newRelayInverted != relayInverted) {
    Serial.println("[Admin] Relay inversion changed → rebooting in 2s...");
    delay(2000);
    ESP.restart();
  }

  relayInverted = newRelayInverted;

  if (!newMqttEnabled && mqttClient.connected()) {
    mqttClient.disconnect();
    Serial.println("[MQTT] Disabled – disconnected from broker");
  }

  switchMqttClient(mqttTlsEnabled);

  Serial.printf("Settings saved | MQTT enabled=%d | TLS=%d | Ver=%s | SNI=%s\n",
                mqttEnabled, mqttTlsEnabled, mqttTlsVersion.c_str(), mqttSni.c_str());

  performWeatherCheck();

  if (mqttEnabled) connectMQTT();

  server.sendHeader("Location", "/");
  server.send(303);
}

// ────────────────────────────────────────────────
//  OTA Update page
// ────────────────────────────────────────────────
void handleUpdate() {
  if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();

  String html = String(sharedCSS) + R"rawliteral(
  <title>OTA Update</title>
</head>
<body>
  <div class="container">
    <div class="card">
      <h1>OTA Firmware Update</h1>
      <p style="color:#666; margin-bottom:24px;">
        Select a new firmware .bin file to upload.<br>
        <strong>Warning:</strong> Do not interrupt power during update.
      </p>

      <form method="POST" action="/update" enctype="multipart/form-data">
        <div class="field">
          <label for="file">Firmware File (.bin)</label>
          <input type="file" name="update" accept=".bin" required>
        </div>
        <button type="submit">Upload & Update</button>
      </form>

      <div class="back" style="margin-top:32px;">
        <a href="/">Back to main</a>
      </div>
    </div>
  </div>
</body>
</html>
  )rawliteral";

  server.send(200, "text/html", html);
}

// ────────────────────────────────────────────────
//  OTA upload handler
// ────────────────────────────────────────────────
void handleDoUpdate() {
  if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();

  HTTPUpload& upload = server.upload();

  if (upload.status == UPLOAD_FILE_START) {
    String filename = upload.filename;
    if (!filename.endsWith(".bin")) {
      server.send(400, "text/plain", "Only .bin files allowed");
      return;
    }

    Serial.printf("[OTA] Update start: %s\n", filename.c_str());
    if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
      Update.printError(Serial);
      server.send(500, "text/plain", "OTA could not begin");
      return;
    }
  } else if (upload.status == UPLOAD_FILE_WRITE) {
    if (Update.write(upload.buf, upload.currentSize) != upload.currentSize) {
      Update.printError(Serial);
      server.send(500, "text/plain", "OTA write failed");
      return;
    }
  } else if (upload.status == UPLOAD_FILE_END) {
    if (Update.end(true)) {
      Serial.printf("[OTA] Success: %u bytes → rebooting\n", upload.totalSize);
      server.send(200, "text/plain", "Update success - rebooting...");
      delay(1000);
      ESP.restart();
    } else {
      Update.printError(Serial);
      server.send(500, "text/plain", "OTA end failed");
    }
  }
  yield();
}

// ────────────────────────────────────────────────
//  Test OpenWeather + location
// ────────────────────────────────────────────────
void handleTestOWM() {
  if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();

  String statusText = "";
  String loc = locationName;

  if (WiFi.status() != WL_CONNECTED) {
    statusText = "No WiFi connection";
  } else if (owmApiKey.length() < 10) {
    statusText = "API Key missing or too short";
  } else {
    String url = (apiVersion == "3.0") ?
      "https://api.openweathermap.org/data/3.0/onecall?lat=" + owmLat + "&lon=" + owmLon + "&appid=" + owmApiKey + "&units=metric&exclude=current,minutely,alerts" :
      "http://api.openweathermap.org/data/2.5/forecast?lat=" + owmLat + "&lon=" + owmLon + "&appid=" + owmApiKey + "&cnt=1";

    http.begin(url);
    http.setTimeout(15000);
    int code = http.GET();

    if (code == HTTP_CODE_OK) {
      String payload = http.getString();
      statusText = "Connected to OpenWeather " + apiVersion + " OK";

      String city = "";
      int cityPos = payload.indexOf("\"name\":\"");
      if (cityPos != -1) {
        cityPos += 8;
        int end = payload.indexOf("\"", cityPos);
        if (end != -1) city = payload.substring(cityPos, end);
      }

      String country = "";
      int countryPos = payload.indexOf("\"country\":\"");
      if (countryPos != -1) {
        countryPos += 11;
        int end = payload.indexOf("\"", countryPos);
        if (end != -1) country = payload.substring(countryPos, end);
      }

      if (city.length() > 0) {
        loc = city;
        if (country.length() > 0) loc += ", " + country;
        locationName = loc;
        statusText += " - " + loc;
      } else {
        statusText += " - No city name found";
      }
    } else {
      statusText = "Connection failed (HTTP " + String(code) + ")";
    }
    http.end();
  }

  String json = "{\"status\":\"" + statusText + "\",\"location\":\"" + loc + "\"}";
  server.send(200, "application/json", json);
}

// ────────────────────────────────────────────────
//  Manual check
// ────────────────────────────────────────────────
void handleManualCheck() {
  if (!server.authenticate(www_username, www_password)) return server.requestAuthentication();

  performWeatherCheck();

  String mode = use48hMode ? "48h" : "24h";
  server.send(200, "text/plain", "Manual check completed (" + mode + " mode). See last message in settings.");
}

// ────────────────────────────────────────────────
//  Main loop
// ────────────────────────────────────────────────
void loop() {
  checkResetButton();   // Check long-press reset

  if (mqttEnabled) {
    if (!mqttClient.connected()) {
      connectMQTT();
    }
    mqttClient.loop();
  }

  vTaskDelay(100);
}