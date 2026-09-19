/*
 * smart_ev_charger.ino
 * ====================
 * Smart Edge-AI EV Charging Station Optimizer — ESP32 firmware.
 *
 * A simulated 3-bay EV charging station with:
 *   - voltage / current / temperature sensing + power & energy calculation,
 *   - EV plug-in / plug-out simulation (auto events + remote control),
 *   - on-device Edge-AI (arrival probability + charging-duration prediction)
 *     running the models exported by ai/train_models.py,
 *   - ALLOW / THROTTLE / DEFER load-management optimizer,
 *   - MQTT telemetry to ThingsBoard + a public mirror broker for the dashboard,
 *   - ThingsBoard RPC + shared-attribute configuration,
 *   - OLED status screen + bay status LEDs (Wokwi).
 *
 * Run it in Wokwi (wokwi.com) with the files in this folder, or compile to a
 * real ESP32 DevKit. See README.md for the full setup.
 */
#include "config.h"
#include "ev_station.h"
#include "ai_edge.h"
#include "optimizer.h"
#include "tb_mqtt.h"
#include "display.h"

static Station station;

// ------------------------------------------------------------------ //
// timers
// ------------------------------------------------------------------ //
static unsigned long tTick = 0, tOptimizer = 0, tAI = 0, tTelemetry = 0, tStatus = 0, tLedBlink = 0;

// ------------------------------------------------------------------ //
// Bay status LEDs (Wokwi visual feedback)
// ------------------------------------------------------------------ //
static void updateBayLeds(unsigned long now) {
  bool blink = (now / 500UL) % 2;
  bool alarmBlink = (now / 250UL) % 2;

  for (uint8_t i = 0; i < 3; i++) {
    Bay& b = station.bay[i];
    int pin = (i == 0) ? PIN_LED_BAY1 : (i == 1) ? PIN_LED_BAY2 : PIN_LED_BAY3;
    if (b.present && b.state == ST_CHARGING) {
      // solid when ALLOW, slow blink when THROTTLE
      digitalWrite(pin, (b.decision == DEC_THROTTLE && !blink) ? LOW : HIGH);
    } else {
      digitalWrite(pin, LOW);
    }
  }
  digitalWrite(PIN_LED_ALARM, (station.alarms != 0 && alarmBlink) ? HIGH : LOW);
}

// ------------------------------------------------------------------ //
// Telemetry publish (ThingsBoard + mirror broker)
// ------------------------------------------------------------------ //
static void publishAll() {
  DynamicJsonDocument doc(2048);
  doc["uptime_s"] = millis() / 1000UL;
  doc["firmware_version"] = "esp32-1.0.0";
  doc["station_total_power_kw"] = round2(station.stationPowerKw);
  doc["station_total_current_a"] = round1(station.stationCurrentA);
  doc["station_load_pct"] = round1(100.0f * station.stationCurrentA /
                                   (station.maxStationCurrent > 0.01f ? station.maxStationCurrent : 1.0f));
  doc["station_energy_kwh"] = round2(station.stationEnergyKwh);
  doc["max_station_current_a"] = station.maxStationCurrent;
  doc["ambient_temp_c"] = round1(station.ambientC);
  doc["optimizer_mode"] = (station.optimizerMode == 0) ? "AUTO" : "MANUAL";
  doc["busy_rate"] = station.busyRate;
  doc["sim_day"] = station.simDay;
  doc["sim_hour"] = round1(station.simMinuteOfDay / 60.0f);
  doc["sim_time"] = station.simTimeString();
  doc["arrival_probability_pct"] = round1(station.lastArrivalProb * 100.0f);
  doc["predicted_arrivals"] = round2(station.predictedArrivals);
  doc["charging_sessions_active"] = station.activeSessions();
  doc["alarms_active"] = (station.alarms != 0) ? 1 : 0;
  doc["alarm_codes"] = station.alarmCodes();
  doc["wifi_rssi_db"] = WiFi.RSSI();
  doc["mqtt_tb"] = mqtt.tbOk();
  doc["mqtt_mirror"] = mqtt.mirrorOk();

  for (uint8_t i = 0; i < 3; i++) {
    Bay& b = station.bay[i];
    String p = "bay" + String(i + 1) + "_";
    doc[p + "state"] = station.stateStr(i + 1);
    doc[p + "soc"] = round1(b.soc);
    doc[p + "voltage_v"] = round1(b.voltageV);
    doc[p + "current_a"] = round1(b.currentA);
    doc[p + "power_kw"] = round2(b.powerKw);
    doc[p + "energy_kwh"] = round2(b.energyKwh);
    doc[p + "temp_c"] = round1(b.tempC);
    doc[p + "target_soc"] = (int)b.targetSoc;
    doc[p + "predicted_min"] = (int)b.predictedMin;
    doc[p + "time_remaining_min"] = (int)b.timeRemainingMin;
    doc[p + "priority"] = (int)b.priority;
    doc[p + "decision"] = station.decisionStr(i + 1);
    doc[p + "reason"] = b.reason;
    doc[p + "vehicle"] = b.present ? b.vehicle : "";
  }

  char buf[2048];
  serializeJson(doc, buf, sizeof(buf));
  mqtt.publishTelemetry(buf);
}

static void publishAttrState() {
  DynamicJsonDocument doc(512);
  doc["optimizerMode"] = (station.optimizerMode == 0) ? "AUTO" : "MANUAL";
  doc["maxStationCurrent_a"] = station.maxStationCurrent;
  doc["busyRate"] = station.busyRate;
  doc["autoEvents"] = station.autoEvents;
  doc["bay1_enabled"] = station.bay[0].enabled;
  doc["bay2_enabled"] = station.bay[1].enabled;
  doc["bay3_enabled"] = station.bay[2].enabled;
  doc["bay1_limit_a"] = station.bay[0].currentLimitA;
  doc["bay2_limit_a"] = station.bay[1].currentLimitA;
  doc["bay3_limit_a"] = station.bay[2].currentLimitA;
  doc["bay1_priority"] = station.bay[0].priority;
  doc["bay2_priority"] = station.bay[1].priority;
  doc["bay3_priority"] = station.bay[2].priority;
  char buf[512];
  serializeJson(doc, buf, sizeof(buf));
  mqtt.publishAttrState(buf);
}

// ------------------------------------------------------------------ //
// Command dispatcher — handles ThingsBoard RPC + dashboard commands.
// ------------------------------------------------------------------ //
static bool handleCommand(const char* method, const JsonVariantConst& params, JsonDocument& resp) {
  resp["cmd"] = method;
  resp["ok"] = true;

  if (strcmp(method, "getStatus") == 0) {
    resp["firmware"] = "esp32-1.0.0";
    resp["uptime_s"] = millis() / 1000UL;
    resp["mode"] = (station.optimizerMode == 0) ? "AUTO" : "MANUAL";
    resp["maxStationCurrent_a"] = station.maxStationCurrent;
    resp["tb"] = mqtt.tbOk();
    return true;
  }
  if (strcmp(method, "resetEnergy") == 0) {
    station.resetEnergy();
    resp["detail"] = "energy reset";
    return true;
  }

  uint8_t bay = (uint8_t)CONSTRAIN((int)(params["bay"] | 1), 1, 3);

  if (strcmp(method, "plugIn") == 0) {
    const char* vehicle = params["vehicle"] | "Tesla Model 3";
    float soc0 = params["soc0"] | -1.0f;
    float socT = params["socT"] | -1.0f;
    int prio = params["priority"] | 0;
    bool ok = station.plugIn(bay, vehicle, soc0, socT, (uint8_t)prio);
    resp["ok"] = ok;
    resp["detail"] = ok ? "EV plugged in" : "bay already occupied";
  } else if (strcmp(method, "plugOut") == 0) {
    bool ok = station.plugOut(bay);
    resp["ok"] = ok;
    resp["detail"] = ok ? "EV unplugged" : "bay already free";
  } else if (strcmp(method, "setEnabled") == 0) {
    station.setEnabled(bay, (bool)(params["enabled"] | true));
    resp["detail"] = "bay enabled state updated";
  } else if (strcmp(method, "setMode") == 0) {
    const char* mode = params["mode"] | "AUTO";
    station.optimizerMode = (strcmp(mode, "MANUAL") == 0) ? 1 : 0;
    resp["detail"] = station.optimizerMode == 0 ? "optimizer AUTO" : "optimizer MANUAL";
  } else if (strcmp(method, "setMaxCurrent") == 0) {
    station.maxStationCurrent = CONSTRAIN(params["amps"] | 64.0f, 16.0f, 200.0f);
    resp["detail"] = "station rating updated";
  } else if (strcmp(method, "setBayLimit") == 0) {
    station.bay[bay - 1].currentLimitA = CONSTRAIN(params["amps"] | 16.0f, 6.0f, 32.0f);
    resp["detail"] = "bay current limit updated";
  } else if (strcmp(method, "setPriority") == 0) {
    station.bay[bay - 1].priority = (uint8_t)CONSTRAIN((int)(params["priority"] | 2), 1, 3);
    resp["detail"] = "bay priority updated";
  } else if (strcmp(method, "setBusyRate") == 0) {
    station.busyRate = CONSTRAIN(params["rate"] | 1.0f, 0.2f, 3.0f);
    resp["detail"] = "busy rate updated";
  } else if (strcmp(method, "setAutoEvents") == 0) {
    station.autoEvents = (bool)(params["on"] | true);
    resp["detail"] = station.autoEvents ? "auto events ON" : "auto events OFF";
  } else if (strcmp(method, "fault") == 0) {
    station.simulateFault(bay);
    resp["detail"] = "fault simulated";
  } else if (strcmp(method, "clearFault") == 0) {
    station.clearFault(bay);
    resp["detail"] = "fault cleared";
  } else {
    resp["ok"] = false;
    resp["detail"] = "unknown method: " + String(method);
    return false;
  }
  return true;
}

// ------------------------------------------------------------------ //
// small rounding helpers
// ------------------------------------------------------------------ //
float round1(float v) { return (float)((int)(v * 10.0f + (v < 0 ? -0.5f : 0.5f))) / 10.0f; }
float round2(float v) { return (float)((int)(v * 100.0f + (v < 0 ? -0.5f : 0.5f))) / 100.0f; }

// ------------------------------------------------------------------ //
void setup() {
  Serial.begin(115200);
  delay(300);
  Serial.println();
  Serial.println("Smart Edge-AI EV Charging Station Optimizer");
  Serial.println("ESP32 firmware v1.0.0");

  pinMode(PIN_LED_BUILTIN, OUTPUT);
  pinMode(PIN_LED_BAY1, OUTPUT);
  pinMode(PIN_LED_BAY2, OUTPUT);
  pinMode(PIN_LED_BAY3, OUTPUT);
  pinMode(PIN_LED_ALARM, OUTPUT);
  digitalWrite(PIN_LED_BAY1, LOW);
  digitalWrite(PIN_LED_BAY2, LOW);
  digitalWrite(PIN_LED_BAY3, LOW);
  digitalWrite(PIN_LED_ALARM, LOW);

  display.begin();

  // start the demo with one EV already charging so the dashboard is alive
  station.plugIn(1, "Tesla Model 3");

  mqtt.setCmdHandler(handleCommand);
  mqtt.setOnAttrsApplied(publishAttrState);
  mqtt.begin();

  refreshEdgeAI(station);
  runOptimizer(station);

  tTick = tOptimizer = tAI = tTelemetry = tStatus = millis();
  Serial.printf("Station rating : %.0f A, mode AUTO\n", station.maxStationCurrent);
  Serial.printf("Arrival prob (start): %.1f%%\n", station.lastArrivalProb * 100.0f);
}

// ------------------------------------------------------------------ //
void loop() {
  unsigned long now = millis();
  mqtt.loop();
  digitalWrite(PIN_LED_BUILTIN, (now / 1000UL) % 2);

  if (now - tTick >= TICK_MS) {
    tTick = now;
    station.step(1.0f);       // +1 simulated minute per real second
    updateBayLeds(now);
  }
  if (now - tOptimizer >= OPTIMIZER_EVERY_MS) {
    tOptimizer = now;
    runOptimizer(station);
  }
  if (now - tAI >= AI_EVERY_MS) {
    tAI = now;
    refreshEdgeAI(station);
  }
  if (now - tTelemetry >= TELEMETRY_EVERY_MS) {
    tTelemetry = now;
    publishAll();
  }
  if (now - tStatus >= 20000UL) {
    tStatus = now;
    StaticJsonDocument<256> stDoc;
    stDoc["uptime_s"] = millis() / 1000UL;
    stDoc["wifi_rssi_db"] = WiFi.RSSI();
    stDoc["tb"] = mqtt.tbOk();
    stDoc["mirror"] = mqtt.mirrorOk();
    stDoc["fw"] = "esp32-1.0.0";
    char sbuf[256];
    serializeJson(stDoc, sbuf, sizeof(sbuf));
    mqtt.publishStatus(sbuf);
  }

  display.render(station, mqtt.tbOk(), mqtt.mirrorOk());
  (void)tLedBlink;
}