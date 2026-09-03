/*
 * Sonoff Basic 1 — ESP8266 MQTT relay switch
 * Raw Arduino C++ + MQTT (QoS 1) + Home Assistant auto-discovery
 *
 * Converted from sonoff-basic-1.yaml (ESPHome), mirroring plug_light1
 * (plugin_light.ino) exactly: plain-text state/command topics (no JSON
 * payloads, no ArduinoJson dependency), manual String concatenation for
 * the HA discovery JSON, and the same WiFi/MQTT connect/retry structure.
 *
 *   - espMqttClient, QoS 1 with PUBACK confirmation
 *   - exponential backoff on MQTT reconnect; timeout/retry on WiFi
 *   - LWT availability topic
 *   - retained HA MQTT discovery configs (device bundles switch + diagnostics)
 *   - diagnostic sensors: WiFi signal (RSSI), MQTT fail count, reset reason
 *   - relay state persisted in flash (EEPROM) so it resumes its last state
 *     after a power cut — see README for why this differs from the source
 *     YAML's default
 *   - status LED mirrors relay state; physical button toggles the relay
 *     locally, same as the source YAML
 *   - DHCP only (no WiFi.config() static IP) — use a router-side DHCP
 *     reservation for a stable address, see README
 *   - manufacturer "P@cho"
 *
 * Hardware: ESP8266 (Sonoff Basic R1/R2, 1MB flash). Relay on GPIO12
 * (active-high), status LED on GPIO13 (active-low, onboard blue LED),
 * button on GPIO0 (active-low).
 *
 * Libraries needed (Arduino IDE > Library Manager):
 *   - espMqttClient (bertmelis)
 *   Built-in / come with the ESP8266 Arduino core: ESP8266WiFi, ArduinoOTA,
 *   EEPROM (ESP8266 has no Preferences/NVS library — that's ESP32-only —
 *   so persistence uses EEPROM emulation instead, see README)
 *
 * Board package: esp8266 by ESP8266 Community — board "Generic ESP8266
 * Module" (or your specific Sonoff Basic entry if your board manager has
 * one). Rename config.h.example -> config.h and fill in your WiFi/MQTT/OTA
 * credentials, device identity, and hardware settings before building.
 */

#include <ESP8266WiFi.h>
#include <ArduinoOTA.h>
#include <espMqttClient.h>
#include <EEPROM.h>

#include "config.h"

// ------------------------------------------------------------------
// MQTT / discovery topics — plain text payloads, no JSON, matching
// plug_light1's topic layout (domain-prefixed base topic, /state and
// /set per control, diagnostics under their own /state subtopics).
// ------------------------------------------------------------------
String baseTopic          = String("switch/") + DEVICE_ID;
String stateTopic         = baseTopic + "/state";        // "ON" / "OFF"
String commandTopic       = baseTopic + "/set";           // "ON" / "OFF"
String availabilityTopic  = baseTopic + "/availability";  // "online" / "offline" (LWT)
String wifiSignalTopic    = baseTopic + "/wifi_signal/state";
String resetReasonTopic   = baseTopic + "/reset_reason/state";
String failCountTopic     = baseTopic + "/mqtt_fail_count/state";

String discoverySwitchTopic      = String("homeassistant/switch/") + DEVICE_ID + "/config";
String discoveryWifiSignalTopic  = String("homeassistant/sensor/") + DEVICE_ID + "/wifi_signal/config";
String discoveryResetReasonTopic = String("homeassistant/sensor/") + DEVICE_ID + "/reset_reason/config";
String discoveryFailCountTopic   = String("homeassistant/sensor/") + DEVICE_ID + "/mqtt_fail_count/config";

// ------------------------------------------------------------------
// Globals
// ------------------------------------------------------------------
espMqttClient mqttClient;

bool relayState = false;

// EEPROM persistence — one byte, relay state only. ESP8266's Arduino core
// has no Preferences/NVS library, so this is the platform-appropriate
// stand-in for plug_light1's Preferences use.
#define EEPROM_SIZE       4
#define EEPROM_ADDR_RELAY 0

// mqtt reconnect / diagnostics
unsigned long lastMqttAttemptMs = 0;
unsigned long mqttBackoffMs     = 1000;
static const unsigned long MQTT_BACKOFF_MAX_MS = 30000;
uint32_t mqttFailCount = 0;
bool     everConnected = false;

unsigned long lastDiagPublishMs = 0;
static const unsigned long DIAG_INTERVAL_MS = 120000; // 2 min, matches plug_light1/smart_switch cadence

// wifi reconnect state (non-blocking)
bool          wifiConnectInProgress = false;
unsigned long wifiConnectStartMs    = 0;
static const unsigned long WIFI_CONNECT_TIMEOUT_MS = 15000;

// button debounce (non-blocking)
int lastButtonReading = HIGH;
int buttonState        = HIGH;
unsigned long lastDebounceMs = 0;

// ------------------------------------------------------------------
// Forward declarations
// ------------------------------------------------------------------
void connectWifi();
void maintainWifi();
bool checkedPublish(const String &topic, uint8_t qos, bool retain, const String &payload);
void connectMqtt();
void onMqttConnect(bool sessionPresent);
void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason);
void onMqttMessage(const espMqttClientTypes::MessageProperties& properties,
                    const char* topic, const uint8_t* payload, size_t len,
                    size_t index, size_t total);
void publishDiscovery();
void publishState();
void setRelay(bool on, bool publish);
void applyRelayOutput(bool on);
void publishDiagnostics(bool force);
void loadPersistedState();
void savePersistedState();
void handleButton();
void setupOta();

// ------------------------------------------------------------------
// Relay / status LED
//
// Status LED always mirrors relay state, whatever triggered the change
// (MQTT command or the physical button) — matches the source YAML's
// on_turn_on/on_turn_off automations on the relay switch, which drove the
// LED unconditionally.
// ------------------------------------------------------------------
void applyRelayOutput(bool on) {
  digitalWrite(RELAY_PIN, on ? HIGH : LOW);
  digitalWrite(STATUS_LED_PIN, on ? LOW : HIGH); // active-low
}

void loadPersistedState() {
  EEPROM.begin(EEPROM_SIZE);
  relayState = EEPROM.read(EEPROM_ADDR_RELAY) == 1;
}

void savePersistedState() {
  EEPROM.write(EEPROM_ADDR_RELAY, relayState ? 1 : 0);
  EEPROM.commit();
}

void setRelay(bool on, bool publish) {
  relayState = on;
  applyRelayOutput(relayState);
  savePersistedState();
  if (publish) publishState();
}

void publishState() {
  if (!mqttClient.connected()) return;
  checkedPublish(stateTopic, 1, true, relayState ? "ON" : "OFF");
}

// ------------------------------------------------------------------
// Button (local toggle) — no equivalent in plug_light1, this device's
// own physical control, kept in the same non-blocking millis() style as
// the rest of the sketch.
// ------------------------------------------------------------------
void handleButton() {
  int reading = digitalRead(BUTTON_PIN);

  if (reading != lastButtonReading) {
    lastDebounceMs = millis();
  }

  if ((millis() - lastDebounceMs) > BUTTON_DEBOUNCE_MS) {
    if (reading != buttonState) {
      buttonState = reading;
      if (buttonState == LOW) { // pressed
        setRelay(!relayState, true);
      }
    }
  }

  lastButtonReading = reading;
}

// ------------------------------------------------------------------
// MQTT publish helper — logs a failure instead of silently dropping it.
// mqttClient.publish() returns 0 on failure (e.g. espMqttClient's internal
// low-memory guard, or not connected) — every call site goes through this
// so a failed publish is always visible on the Serial Monitor.
// ------------------------------------------------------------------
bool checkedPublish(const String &topic, uint8_t qos, bool retain, const String &payload) {
  uint16_t packetId = mqttClient.publish(topic.c_str(), qos, retain, payload.c_str());
  if (packetId == 0) {
    Serial.print("[mqtt] publish FAILED topic=");
    Serial.print(topic);
    Serial.print(" free_heap=");
    Serial.println(ESP.getFreeHeap());
  }
  return packetId != 0;
}

// ------------------------------------------------------------------
// Home Assistant discovery — manual String JSON, no ArduinoJson.
// ------------------------------------------------------------------
void publishDiscovery() {
  String deviceJson = String("{") +
      "\"identifiers\":[\"" + DEVICE_ID + "\"]," +
      "\"name\":\"" + DEVICE_NAME + "\"," +
      "\"manufacturer\":\"" + MANUFACTURER + "\"," +
      "\"model\":\"" + MODEL + "\"," +
      "\"sw_version\":\"" + FW_VERSION + "\"" +
      "}";

  // Switch (plain MQTT switch schema)
  {
    String payload = String("{") +
        "\"name\":\"" + DEVICE_NAME + "\"," +
        "\"unique_id\":\"" + DEVICE_ID + "\"," +
        "\"state_topic\":\"" + stateTopic + "\"," +
        "\"command_topic\":\"" + commandTopic + "\"," +
        "\"payload_on\":\"ON\"," +
        "\"payload_off\":\"OFF\"," +
        "\"availability_topic\":\"" + availabilityTopic + "\"," +
        "\"device\":" + deviceJson +
        "}";
    checkedPublish(discoverySwitchTopic, 1, true, payload);
  }

  // WiFi signal (diagnostic)
  {
    String payload = String("{") +
        "\"name\":\"WiFi Signal\"," +
        "\"unique_id\":\"" + DEVICE_ID + "_wifi_signal\"," +
        "\"state_topic\":\"" + wifiSignalTopic + "\"," +
        "\"unit_of_measurement\":\"dBm\"," +
        "\"device_class\":\"signal_strength\"," +
        "\"state_class\":\"measurement\"," +
        "\"entity_category\":\"diagnostic\"," +
        "\"availability_topic\":\"" + availabilityTopic + "\"," +
        "\"device\":" + deviceJson +
        "}";
    checkedPublish(discoveryWifiSignalTopic, 1, true, payload);
  }

  // Reset reason (diagnostic)
  {
    String payload = String("{") +
        "\"name\":\"Reset Reason\"," +
        "\"unique_id\":\"" + DEVICE_ID + "_reset_reason\"," +
        "\"state_topic\":\"" + resetReasonTopic + "\"," +
        "\"entity_category\":\"diagnostic\"," +
        "\"availability_topic\":\"" + availabilityTopic + "\"," +
        "\"device\":" + deviceJson +
        "}";
    checkedPublish(discoveryResetReasonTopic, 1, true, payload);
  }

  // MQTT fail count (diagnostic)
  {
    String payload = String("{") +
        "\"name\":\"MQTT Fail Count\"," +
        "\"unique_id\":\"" + DEVICE_ID + "_mqtt_fail_count\"," +
        "\"state_topic\":\"" + failCountTopic + "\"," +
        "\"entity_category\":\"diagnostic\"," +
        "\"state_class\":\"total_increasing\"," +
        "\"availability_topic\":\"" + availabilityTopic + "\"," +
        "\"device\":" + deviceJson +
        "}";
    checkedPublish(discoveryFailCountTopic, 1, true, payload);
  }
}

// ------------------------------------------------------------------
// MQTT callbacks
// ------------------------------------------------------------------
void onMqttConnect(bool sessionPresent) {
  Serial.println("MQTT connected");
  mqttBackoffMs = 1000; // reset backoff on success
  everConnected = true;

  checkedPublish(availabilityTopic, 1, true, "online");

  mqttClient.subscribe(commandTopic.c_str(), 1);

  publishDiscovery();
  publishState();
  publishDiagnostics(true);
}

void onMqttDisconnect(espMqttClientTypes::DisconnectReason reason) {
  Serial.printf("MQTT disconnected, reason: %u\n", static_cast<uint8_t>(reason));
  if (everConnected) {
    mqttFailCount++;
  }
  mqttBackoffMs = min(mqttBackoffMs * 2, MQTT_BACKOFF_MAX_MS);
}

void onMqttMessage(const espMqttClientTypes::MessageProperties& properties,
                    const char* topic, const uint8_t* payload, size_t len,
                    size_t index, size_t total) {
  String topicStr(topic);
  String payloadStr;
  payloadStr.reserve(len);
  for (size_t i = 0; i < len; i++) payloadStr += (char)payload[i];

  Serial.print("[mqtt] message topic=");
  Serial.print(topicStr);
  Serial.print(" payload=");
  Serial.println(payloadStr);

  if (topicStr == commandTopic) {
    bool wantOn = payloadStr.equalsIgnoreCase("ON");
    setRelay(wantOn, true);
  }
}

// ------------------------------------------------------------------
// WiFi — DHCP only, no WiFi.config() static IP call. Non-blocking
// timeout/retry, mirrors plug_light1/smart_switch.
// ------------------------------------------------------------------
void connectWifi() {
  if (WiFi.status() == WL_CONNECTED || wifiConnectInProgress) return;

  WiFi.mode(WIFI_STA);
  WiFi.hostname(DEVICE_ID); // ESP8266 API: hostname(), not setHostname()
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  wifiConnectInProgress = true;
  wifiConnectStartMs = millis();
  Serial.println("[wifi] connecting...");
}

// Non-blocking — call every loop() iteration. Starts a connect attempt if
// needed, and lets a stuck attempt time out and retry without stalling the
// rest of loop() (MQTT, OTA) the way a blocking wait would.
void maintainWifi() {
  if (WiFi.status() == WL_CONNECTED) {
    if (wifiConnectInProgress) {
      wifiConnectInProgress = false;
      Serial.printf("[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
    }
    return;
  }

  if (!wifiConnectInProgress) {
    connectWifi();
  } else if (millis() - wifiConnectStartMs > WIFI_CONNECT_TIMEOUT_MS) {
    Serial.println("[wifi] connect attempt timed out, will retry");
    wifiConnectInProgress = false; // next call to maintainWifi() starts a fresh attempt
  }
}

// ------------------------------------------------------------------
// MQTT connection maintenance (exponential backoff)
// ------------------------------------------------------------------
void connectMqtt() {
  lastMqttAttemptMs = millis();
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.println("Connecting to MQTT...");
  mqttClient.connect();
}

// ------------------------------------------------------------------
// Diagnostics
// ------------------------------------------------------------------
void publishDiagnostics(bool force) {
  if (!mqttClient.connected()) return;

  if (WiFi.status() == WL_CONNECTED) {
    long rssi = WiFi.RSSI();
    checkedPublish(wifiSignalTopic, 1, true, String(rssi));
  }

  static bool resetReasonSent = false;
  if (force || !resetReasonSent) {
    checkedPublish(resetReasonTopic, 1, true, ESP.getResetReason());
    resetReasonSent = true;
  }

  checkedPublish(failCountTopic, 1, true, String(mqttFailCount));
}

// ------------------------------------------------------------------
// OTA
// ------------------------------------------------------------------
void setupOta() {
  ArduinoOTA.setHostname(DEVICE_ID);
  ArduinoOTA.setPassword(OTA_PASSWORD);
  ArduinoOTA.onStart([]() { Serial.println("[OTA] start"); });
  ArduinoOTA.onEnd([]() { Serial.println("[OTA] done"); });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("[OTA] error %u\n", (unsigned)error);
  });
  ArduinoOTA.begin();
}

// ------------------------------------------------------------------
// Setup / loop
// ------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println("\n[Sonoff Basic 1] booting, reset reason: " + ESP.getResetReason());

  pinMode(RELAY_PIN, OUTPUT);
  pinMode(STATUS_LED_PIN, OUTPUT);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  loadPersistedState();
  applyRelayOutput(relayState); // snap to persisted state instantly, no fade for a relay

  connectWifi();

  // One-time bounded wait at boot only — gives ArduinoOTA's mDNS responder
  // and the first MQTT attempt a real chance at a live link. This never
  // recurs after setup(), so it never blocks loop() the way a recurring
  // blocking wait would.
  {
    unsigned long waitStart = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - waitStart < 5000) {
      delay(100);
    }
  }
  if (WiFi.status() == WL_CONNECTED) {
    wifiConnectInProgress = false;
    Serial.printf("[wifi] connected, ip=%s\n", WiFi.localIP().toString().c_str());
  } else {
    Serial.println("[wifi] not yet connected at boot, will keep retrying in loop()");
  }

  mqttClient.setServer(MQTT_HOST, MQTT_PORT);
  mqttClient.setCredentials(MQTT_USER, MQTT_PASSWORD);
  mqttClient.setClientId(DEVICE_ID);
  mqttClient.setWill(availabilityTopic.c_str(), 1, true, "offline");
  mqttClient.onConnect(onMqttConnect);
  mqttClient.onDisconnect(onMqttDisconnect);
  mqttClient.onMessage(onMqttMessage);
  connectMqtt();

  setupOta();
}

void loop() {
  maintainWifi();

  if (!mqttClient.connected()) {
    unsigned long now = millis();
    if (now - lastMqttAttemptMs >= mqttBackoffMs) {
      connectMqtt();
    }
  }

  mqttClient.loop();
  ArduinoOTA.handle();
  handleButton();

  unsigned long now = millis();
  if (now - lastDiagPublishMs >= DIAG_INTERVAL_MS) {
    lastDiagPublishMs = now;
    publishDiagnostics(false);
  }
}
