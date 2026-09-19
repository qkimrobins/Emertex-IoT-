/*
 * tb_mqtt.h
 * =========
 * MQTT layer talking to ThingsBoard (device API) AND to a public "mirror"
 * broker that the browser dashboard listens on.
 *
 * ThingsBoard device MQTT API implemented here:
 *   - telemetry  : publish  -> v1/devices/me/telemetry
 *   - attributes : subscribe -> v1/devices/me/attributes   (shared attrs)
 *                  request   -> v1/devices/me/attributes/request/1
 *                  response  <- v1/devices/me/attributes/response/1
 *   - RPC        : subscribe -> v1/devices/me/rpc/request/+
 *                  respond   -> v1/devices/me/rpc/response/<requestId>
 *
 * Dashboard protocol (mirror broker):
 *   - telemetry  : publish    -> emx/ev/telemetry
 *   - commands   : subscribe  <- emx/ev/cmd   ({"cmd": "...", "params": {...}})
 *   - ack        : publish    -> emx/ev/cmd/ack
 *   - attr state : publish    -> emx/ev/attr/state
 *   - status     : publish    -> emx/ev/status
 */
#pragma once

#include <Arduino.h>

// Larger MQTT buffers for our telemetry payloads (must be set BEFORE the lib)
#ifndef MQTT_MAX_PACKET_SIZE
#define MQTT_MAX_PACKET_SIZE 2048
#endif
#ifndef MQTT_KEEPALIVE
#define MQTT_KEEPALIVE 30
#endif

#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>

#include "config.h"

// Command dispatcher: implement in the sketch. Returns true on success and
// fills `resp` with the acknowledgement payload.
typedef bool (*CmdHandler)(const char* method, const JsonVariantConst& params, JsonDocument& resp);

class MqttManager {
 public:
  void begin();                       // WiFi + connect both brokers
  void loop();                        // maintain connections + process messages
  void setCmdHandler(CmdHandler h) { handler_ = h; }

  bool tbOk() { return tb_.connected(); }
  bool mirrorOk() { return mirror_.connected(); }

  void publishTelemetry(const char* json);   // to TB + mirror
  void publishAttrState(const char* json);   // to mirror (+ TB attributes topic)
  void publishStatus(const char* json);      // to mirror

 private:
  WiFiClient wifiTb_;
  WiFiClient wifiMirror_;
  PubSubClient tb_;
  PubSubClient mirror_;
  CmdHandler handler_ = nullptr;

  unsigned long wifiRetryMs_ = 0;
  unsigned long tbReconnectMs_ = 0;
  unsigned long mirrorReconnectMs_ = 0;
  bool attrRequested_ = false;

  void connectWifi();
  void connectTb();
  void connectMirror();
  void subscribeTb();
  void handleMessage(PubSubClient& client, const char* topic,
                     const byte* payload, unsigned int length);
  void handleTbRpcRequest(const char* topic, const char* payload);
  void handleTbAttributes(const char* payload);
  void handleMirrorCommand(const char* payload);
  void dispatchCommand(const char* method, const JsonVariantConst& params,
                       PubSubClient& replyClient, const char* replyTopic);
  void dispatchLocal(const char* method, const JsonVariantConst& params);

  static void onTbMessage(char* topic, byte* payload, unsigned int length);
  static void onMirrorMessage(char* topic, byte* payload, unsigned int length);

 public:
  // called after shared attributes changed the station configuration
  void setOnAttrsApplied(void (*fn)()) { onAttrsApplied_ = fn; }

 private:
  void (*onAttrsApplied_)() = nullptr;
};

// single global instance (defined in tb_mqtt.cpp)
extern MqttManager mqtt;