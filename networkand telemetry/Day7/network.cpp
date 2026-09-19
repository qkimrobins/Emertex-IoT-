#include <WiFi.h>
#include "network.h"
#include "config.h"


WiFiClient espClient;
PubSubClient mqtt(espClient);

// ---------------------------------------------------------------------
void connectWiFi() {
  Serial.print("Connecting to WiFi");
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println(WiFi.status() == WL_CONNECTED ? " connected." : " FAILED (will retry).");
}

void connectMQTT() {
  if (WiFi.status() != WL_CONNECTED) return;
  Serial.print("Connecting to ThingsBoard MQTT...");
  if (mqtt.connect(BAY_ID, TB_TOKEN, NULL)) {
    Serial.println(" connected.");
    
  } else {
    Serial.print(" failed, rc=");
    Serial.println(mqtt.state());
    delay(1000);
  }
}

// ---------------------------------------------------------------------
// Single MQTT callback, dispatched by topic: attribute push/response vs.
// RPC request. This is where Phase 6's two device-side features live.
// ---------------------------------------------------------------------
