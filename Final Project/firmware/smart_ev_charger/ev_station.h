/*
 * ev_station.h
 * ============
 * The simulated EV charging station: 3 bays with electrical sensing, charging
 * profile and thermal model. This replaces real hardware sensors on the Wokwi
 * prototype, but the interface (voltage / current / temperature / energy) is
 * exactly what a physical station with CTs + NTC sensors would provide.
 */
#pragma once

#include <Arduino.h>
#include "config.h"

// ------------------------------------------------------------------ //
// Bay state machine
// ------------------------------------------------------------------ //
enum BayState : uint8_t { ST_IDLE = 0, ST_CHARGING = 1, ST_DONE = 2 };

enum Decision : uint8_t { DEC_ALLOW = 0, DEC_THROTTLE = 1, DEC_DEFER = 2 };

enum AlarmBits : uint8_t {
  AL_TEMP_HIGH = 1 << 0,
  AL_TEMP_CRIT = 1 << 1,
  AL_OVERLOAD = 1 << 2,
  AL_VOLT_LOW = 1 << 3,
  AL_SIM_FAULT = 1 << 4,
};

struct Bay {
  uint8_t index = 0;
  bool enabled = true;       // socket enabled (config)
  bool present = false;      // an EV is physically plugged in
  bool faulted = false;      // locked by a fault
  uint8_t state = ST_IDLE;

  // measured / simulated electrical values
  float voltageV = V_NOM;
  float currentA = 0.0f;
  float tempC = 25.0f;       // connector / cable temperature
  float powerKw = 0.0f;
  float energyKwh = 0.0f;    // session energy

  // session parameters
  float soc = 0.0f;          // state of charge %
  float targetSoc = 90.0f;
  float capacityKwh = 60.0f;
  float currentLimitA = BAY_CURRENT_LIMIT_DEFAULT;   // socket rating
  uint8_t priority = 2;      // 1 = highest
  const char* vehicle = "";

  // Edge-AI outputs
  float predictedMin = 0.0f;      // predicted full session duration (min)
  float timeRemainingMin = 0.0f;  // live estimate (min)

  // optimizer outputs
  float allowedA = 0.0f;          // current this bay may draw
  uint8_t decision = DEC_ALLOW;
  const char* reason = "idle";

  bool isCharging() const { return state == ST_CHARGING; }
};

struct VehiclePreset {
  const char* name;
  float capacityKwh;
  float soc0Lo, soc0Hi;
  float targetSoc;
  float currentLimitA;
};

// A small fleet profile so the simulator produces realistic sessions.
static const VehiclePreset VEHICLES[] = {
    {"Tesla Model 3",   60.0f, 15, 45, 90,  32.0f},
    {"Tesla Model Y",   75.0f, 20, 50, 90,  32.0f},
    {"Hyundai IONIQ 5", 77.0f, 25, 55, 95,  26.0f},
    {"Nissan Leaf",     40.0f, 20, 60, 100, 32.0f},
    {"VW ID.4",         82.0f, 10, 50, 90,  30.0f},
    {"Porsche Taycan",  93.0f, 15, 45, 85,  32.0f},
    {"BYD Atto 3",      60.0f, 20, 50, 95,  32.0f},
    {"Audi e-tron GT",  85.0f, 10, 40, 90,  32.0f},
};

struct Station {
  Bay bay[3];
  float maxStationCurrent = STATION_MAX_CURRENT_DEFAULT;
  uint8_t optimizerMode = 0;      // 0 AUTO, 1 MANUAL
  float busyRate = 1.0f;          // arrival-rate multiplier (shared attr)
  bool autoEvents = true;         // simulator plugs EVs in/out itself
  float ambientBase = 22.0f;

  // derived electrical totals
  float ambientC = 22.0f;
  float stationCurrentA = 0.0f;
  float stationPowerKw = 0.0f;
  float stationEnergyKwh = 0.0f;

  // demo clock
  uint32_t simDay = SIM_DAY_START;          // 0..6
  float simMinuteOfDay = SIM_MINUTE_START;  // 0..1439.9

  // Edge-AI + optimizer state
  float lastArrivalProb = 0.05f;
  float predictedArrivals = 0.0f;
  uint8_t alarms = 0;

  Station();

  // --- simulation ------------------------------------------------- //
  void step(float dtMin);                 // advance the electrical model
  void resetDay();
  void resetEnergy();

  // --- fleet control API ------------------------------------------ //
  bool plugIn(uint8_t idx, const char* vehicleName,
              float soc0 = -1, float targetSoc = -1, uint8_t priority = 0);
  bool plugOut(uint8_t idx);
  void simulateFault(uint8_t idx);
  void clearFault(uint8_t idx);
  void setEnabled(uint8_t idx, bool en);

  // --- auto events (driven by the Edge-AI arrival probability) ----- //
  void runAutoEvents(float dtMin);

  float mainsVoltage() const;
  const char* decisionStr(uint8_t idx) const;
  const char* stateStr(uint8_t idx) const;
  const char* alarmCodes() const;          // "TEMP_HIGH|OVERLOAD" style
  const char* simTimeString() const;       // "Wed 14:32" (demo clock)
  int activeSessions() const;              // number of bays charging
};

// Deterministic PRNG used by the simulator (xorshift32).
extern uint32_t simRng;
inline float randRange(float lo, float hi) {
  simRng ^= simRng << 13; simRng ^= simRng >> 17; simRng ^= simRng << 5;
  return lo + (hi - lo) * (simRng / 4294967296.0f);
}
inline uint32_t randU32(uint32_t n) {  // [0, n)
  simRng ^= simRng << 13; simRng ^= simRng >> 17; simRng ^= simRng << 5;
  return simRng % n;
}