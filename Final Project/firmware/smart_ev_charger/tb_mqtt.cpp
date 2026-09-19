/*
 * tb_mqtt.cpp
 * ===========
 * MQTT plumbing for ThingsBoard + the public mirror broker.
 */
#include "tb_mqtt.h"
#include <WiFi.h>

MqttManager mqtt;

// ------------------------------------------------------------------ //
void MqttManager::connectWifi() {
  if (WiFi.status() == WL_CONNECTED) return;
  if (millis() < wifiRetryMs_) return;

  Serial.printf("[wifi] connecting to %s ...\n", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  wifiRetryMs_ = millis() + 15000UL;
}

// ------------------------------------------------------------------ //
void MqttManager::begin() {
  tb_.setServer(TB_HOST, TB_PORT);
  tb_.setCallback(onTbMessage);
  mirror_.setServer(MIRROR_HOST, MIRROR_PORT);
  mirror_.setCallback(onMirrorMessage);
  connectWifi();
}

// ------------------------------------------------------------------ //
void MqttManager::loop() {
  connectWifi();
  if (WiFi.status() == WL_CONNECTED) {
    if (!tb_.connected()) {
      if (millis() >= tbReconnectMs_) {
        connectTb();
        tbReconnectMs_ = millis() + 8000UL;
      }
    } else {
      tb_.loop();
    }

    if (!mirror_.connected()) {
      if (millis() >= mirrorReconnectMs_) {
        connectMirror();
        mirrorReconnectMs_ = millis() + 8000UL;
      }
    } else {
      mirror_.loop();
    }
  }
}

// ------------------------------------------------------------------ //
void MqttManager::connectTb() {
  #if TB_ENABLED
  Serial.printf("[mqtt] TB %s:%d ...\n", TB_HOST, TB_PORT);
  String clientId = "smart-ev-station-1";
  if (tb_.connect(clientId.c_str(), TB_ACCESS_TOKEN, nullptr)) {
    Serial.println("[mqtt] TB connected");
    subscribeTb();
    attrRequested_ = false;
  } else {
    Serial.printf("[mqtt] TB connect failed rc=%d\n", tb_.state());
  }
  #else
  (void)0;
  #endif
}

void MqttManager::subscribeTb() {
  tb_.subscribe(TOPIC_TB_ATTRIBUTES);
  tb_.subscribe(TOPIC_TB_RPC_REQ);
  tb_.subscribe("v1/devices/me/attributes/response/+");
}

void MqttManager::connectMirror() {
  Serial.printf("[mqtt] mirror %s:%d ...\n", MIRROR_HOST, MIRROR_PORT);
  String clientId = "smart-ev-station-" + String((uint32_t)ESP.getEfuseMac());
  if (mirror_.connect(clientId.c_str())) {
    Serial.println("[mqtt] mirror connected");
    mirror_.subscribe(TOPIC_MIRROR_COMMAND);
  } else {
    Serial.printf("[mqtt] mirror connect failed rc=%d\n", mirror_.state());
  }
}

// ------------------------------------------------------------------ //
void MqttManager::publishTelemetry(const char* json) {
  if (tb_.connected()) tb_.publish(TOPIC_TB_TELEMETRY, json);
  if (mirror_.connected()) mirror_.publish(TOPIC_MIRROR_TELEMETRY, json);
}

void MqttManager::publishAttrState(const char* json) {
  if (tb_.connected()) tb_.publish(TOPIC_TB_ATTRIBUTES, json);   // client attrs
  if (mirror_.connected()) mirror_.publish(TOPIC_MIRROR_ATTR_STATE, json);
}

void MqttManager::publishStatus(const char* json) {
  if (mirror_.connected()) mirror_.publish(TOPIC_MIRROR_STATUS, json);
}

// ------------------------------------------------------------------ //
// Message routing
// ------------------------------------------------------------------ //
void MqttManager::onTbMessage(char* topic, byte* payload, unsigned int length) {
  mqtt.handleMessage(mqtt.tb_, topic, payload, length);
}

void MqttManager::onMirrorMessage(char* topic, byte* payload, unsigned int length) {
  mqtt.handleMessage(mqtt.mirror_, topic, payload, length);
}

void MqttManager::handleMessage(PubSubClient& client, const char* topic,
                                const byte* payload, unsigned int length) {
  if (length >= MQTT_MAX_PACKET_SIZE - 16) length = MQTT_MAX_PACKET_SIZE - 16;
  char buf[MQTT_MAX_PACKET_SIZE];
  memcpy(buf, payload, length);
  buf[length] = '\0';

  if (strncmp(topic, "v1/devices/me/rpc/request/", 26) == 0) {
    handleTbRpcRequest(topic, buf);
  } else if (strncmp(topic, "v1/devices/me/attributes", 24) == 0) {
    handleTbAttributes(buf);
  } else if (strcmp(topic, TOPIC_MIRROR_COMMAND) == 0) {
    handleMirrorCommand(buf);
  }
  (void)client;
}

// ------------------------------------------------------------------ //
void MqttManager::handleTbRpcRequest(const char* topic, const char* payload) {
  const char* idStr = strrchr(topic, '/');
  if (!idStr) return;
  idStr++;  // skip '/'

  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, payload)) return;
  const char* method = doc["method"] | "";
  JsonVariantConst params = doc["params"];

  char replyTopic[64];
  snprintf(replyTopic, sizeof(replyTopic), "%s%s", TOPIC_TB_RPC_RESP, idStr);
  dispatchCommand(method, params, tb_, replyTopic);
}

// ------------------------------------------------------------------ //
// Shared attributes arrive either as {"shared": {...}} (update pushed by TB)
// or as a flat object (response to our attribute request).
void MqttManager::handleTbAttributes(const char* payload) {
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, payload)) return;

  JsonVariant target;
  if (doc.containsKey("shared")) target = doc["shared"];
  else target = doc.as<JsonVariant>();
  if (!target.is<JsonObject>()) return;

  // request current shared attributes once after (re)connect
  if (!attrRequested_) {
    tb_.publish("v1/devices/me/attributes/request/1", "{\"sharedKeys\":[\"*\"]}");
    attrRequested_ = true;
  }

  JsonObject attrs = target.as<JsonObject>();
  bool changed = false;
  for (JsonPair kv : attrs) {
    const char* key = kv.key().c_str();
    JsonVariantConst v = kv.value();
    // map TB shared attribute key -> our command API
    const char* method = nullptr;
    if      (strcmp(key, "optimizerMode") == 0)      method = "setMode";
    else if (strcmp(key, "maxStationCurrent_a") == 0) method = "setMaxCurrent";
    else if (strcmp(key, "busyRate") == 0)           method = "setBusyRate";
    else if (strcmp(key, "autoEvents") == 0)         method = "setAutoEvents";
    else if (strncmp(key, "bay", 3) == 0) {
      uint8_t bayNum = 0;
      if      (strcmp(key, "bay1_enabled") == 0)  { bayNum = 1; method = "setEnabled"; }
      else if (strcmp(key, "bay2_enabled") == 0)  { bayNum = 2; method = "setEnabled"; }
      else if (strcmp(key, "bay3_enabled") == 0)  { bayNum = 3; method = "setEnabled"; }
      else if (strcmp(key, "bay1_limit_a") == 0)  { bayNum = 1; method = "setBayLimit"; }
      else if (strcmp(key, "bay2_limit_a") == 0)  { bayNum = 2; method = "setBayLimit"; }
      else if (strcmp(key, "bay3_limit_a") == 0)  { bayNum = 3; method = "setBayLimit"; }
      else if (strcmp(key, "bay1_priority") == 0) { bayNum = 1; method = "setPriority"; }
      else if (strcmp(key, "bay2_priority") == 0) { bayNum = 2; method = "setPriority"; }
      else if (strcmp(key, "bay3_priority") == 0) { bayNum = 3; method = "setPriority"; }
      if (method && bayNum) {
        DynamicJsonDocument p(128);
        p["bay"] = bayNum;
        if (strcmp(method, "setEnabled") == 0) p["enabled"] = v.as<bool>();
        if (strcmp(method, "setBayLimit") == 0) p["amps"] = v.as<float>();
        if (strcmp(method, "setPriority") == 0) p["priority"] = v.as<int>();
        dispatchLocal(method, p.as<JsonVariantConst>());
        changed = true;
        continue;
      }
      method = nullptr;
    }
    if (method) {
      DynamicJsonDocument p(128);
      if      (strcmp(method, "setMode") == 0)      p["mode"] = v.as<const char*>();
      else if (strcmp(method, "setMaxCurrent") == 0) p["amps"] = v.as<float>();
      else if (strcmp(method, "setBusyRate") == 0)  p["rate"] = v.as<float>();
      else if (strcmp(method, "setAutoEvents") == 0) p["on"] = v.as<bool>();
      dispatchLocal(method, p.as<JsonVariantConst>());
      changed = true;
    }
  }
  if (changed && onAttrsApplied_) onAttrsApplied_();
}

// ------------------------------------------------------------------ //
void MqttManager::handleMirrorCommand(const char* payload) {
  DynamicJsonDocument doc(1024);
  if (deserializeJson(doc, payload)) return;
  const char* method = doc["cmd"] | "";
  JsonVariantConst params;
  if (doc.containsKey("params")) params = doc["params"];
  else params = doc.as<JsonVariant>();
  dispatchCommand(method, params, mirror_, TOPIC_MIRROR_ACK);
}

// ------------------------------------------------------------------ //
void MqttManager::dispatchCommand(const char* method, const JsonVariantConst& params,
                                  PubSubClient& replyClient, const char* replyTopic) {
  DynamicJsonDocument resp(512);
  bool ok = false;
  if (handler_) {
    ok = handler_(method, params, resp);
  } else {
    resp["ok"] = false;
    resp["error"] = "no command handler";
  }
  (void)ok;
  char out[512];
  serializeJson(resp, out, sizeof(out));
  if (replyTopic && replyClient.connected()) replyClient.publish(replyTopic, out);
}

// ------------------------------------------------------------------ //
void MqttManager::dispatchLocal(const char* method, const JsonVariantConst& params) {
  if (!handler_) return;
  DynamicJsonDocument resp(128);
  handler_(method, params, resp);
}