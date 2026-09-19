#include <ArduinoJson.h>
#include "telemetry.h"
#include "network.h"
#include "state.h"
#include "config.h"

// ---------------------------------------------------------------------
// FR-7 telemetry publish. Adds `overloadActive` (additive, beyond SRS
// 8.3's baseline schema) so the ThingsBoard Overcurrent alarm rule can
// be a one-line filter. Also adds `manualOverrideActive` so the
// dashboard can show when a bay is under operator control instead of
// automatic optimization.
// ---------------------------------------------------------------------
void publishTelemetry() {
  if (!mqtt.connected()) return;

  StaticJsonDocument<350> doc;
  doc["bayId"] = BAY_ID;
  doc["voltage"] = round(voltage * 10) / 10.0;
  doc["current"] = round(current * 10) / 10.0;
  doc["power"] = round(power * 10) / 10.0;
  doc["temperature"] = round(temperature * 10) / 10.0;
  doc["bayStatus"] = bayStatus;
  

  char buffer[350];
  serializeJson(doc, buffer);

  mqtt.publish("v1/devices/me/telemetry", buffer);
  Serial.print("[MQTT >>] ");
  Serial.println(buffer);
}
