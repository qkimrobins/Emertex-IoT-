/*
 * ev_station.cpp
 * ==============
 * Implementation of the 3-bay electrical simulator.
 *
 * The physics here intentionally matches the Python reference implementation
 * (optimizer/optimizer.py) so the exact same numbers can be reproduced on the
 * host before touching the hardware.
 */
#include "ev_station.h"

uint32_t simRng = 0x9E3779B9U;   // seeded xorshift32 state

Station::Station() {
  for (uint8_t i = 0; i < 3; i++) {
    bay[i].index = i + 1;
    bay[i].priority = i + 1;          // bay 1 highest by default
  }
}

// ------------------------------------------------------------------ //
void Station::step(float dtMin) {
  // advance the demo clock (used by the Edge-AI time-of-day features)
  simMinuteOfDay += dtMin;
  if (simMinuteOfDay >= 1440.0f) {
    simMinuteOfDay -= 1440.0f;
    simDay = (simDay + 1) % 7;
  }

  // 24 h ambient temperature cycle
  float hour = simMinuteOfDay / 60.0f;
  ambientC = ambientBase + 4.0f * sinf((hour - 9.0f) / 24.0f * 2.0f * M_PI);

  // --- per-bay electrical + thermal model ---
  stationCurrentA = 0.0f;
  stationPowerKw = 0.0f;
  float totalEnergyWh = 0.0f;

  for (uint8_t i = 0; i < 3; i++) {
    Bay& b = bay[i];

    if (!b.present) {
      b.currentA = 0.0f; b.powerKw = 0.0f; b.state = ST_IDLE;
      b.voltageV = mainsVoltage();
      b.tempC = ambientC;
      b.timeRemainingMin = 0.0f;
      continue;
    }

    if (b.state == ST_CHARGING) {
      // CC -> CV taper as the pack approaches full
      float taper = (b.soc >= 80.0f) ? CONSTRAIN((100.0f - b.soc) / 20.0f, 0.05f, 1.0f) : 1.0f;
      b.currentA = b.allowedA * taper;
      if (b.currentA < 0.05f) b.currentA = 0.0f;
      b.voltageV = mainsVoltage();
      float pW = b.voltageV * b.currentA * PF * ETA;
      b.powerKw = pW / 1000.0f;
      float dWh = pW * dtMin / 60.0f;               // Wh added this tick
      b.energyKwh += dWh / 1000.0f;
      totalEnergyWh += dWh;
      b.soc += dWh / (b.capacityKwh * 1000.0f) * 100.0f;
      if (b.soc >= b.targetSoc) {
        b.soc = b.targetSoc;
        b.state = ST_DONE;
        b.currentA = 0.0f; b.powerKw = 0.0f;
      } else {
        // remaining time = energy needed [Wh] / actual power [W] * 60
        float neededWh = b.capacityKwh * 1000.0f * (b.targetSoc - b.soc) / 100.0f;
        b.timeRemainingMin = (pW > 1.0f) ? neededWh / pW * 60.0f : 0.0f;
      }
    } else {  // ST_DONE: keep showing the socket, no power
      b.currentA = 0.0f; b.powerKw = 0.0f;
      b.voltageV = mainsVoltage();
      b.timeRemainingMin = 0.0f;
    }

    // connector / cable temperature: first-order approach to T_eq = f(I^2)
    float teq = ambientC + 5.0f + 0.020f * b.currentA * b.currentA;
    float tau = 1.5f;                                // minutes
    b.tempC += (teq - b.tempC) * CONSTRAIN(dtMin / tau, 0.0f, 1.0f);

    stationCurrentA += b.currentA;
    stationPowerKw += b.powerKw;
  }

  stationEnergyKwh += totalEnergyWh / 1000.0f;

  runAutoEvents(dtMin);
}

// ------------------------------------------------------------------ //
float Station::mainsVoltage() const {
  return V_NOM - 0.05f * stationCurrentA + randRange(-1.0f, 1.0f);
}

void Station::resetDay() { simDay = SIM_DAY_START; simMinuteOfDay = SIM_MINUTE_START; }
void Station::resetEnergy() { stationEnergyKwh = 0.0f; }

// ------------------------------------------------------------------ //
bool Station::plugIn(uint8_t idx, const char* vehicleName,
                     float soc0, float targetSoc, uint8_t priority) {
  if (idx < 1 || idx > 3) return false;
  Bay& b = bay[idx - 1];
  if (b.present) return false;

  const VehiclePreset* spec = &VEHICLES[0];
  for (unsigned i = 0; i < ARRAY_SIZE(VEHICLES); i++) {
    if (strcmp(VEHICLES[i].name, vehicleName) == 0) { spec = &VEHICLES[i]; break; }
  }
  b.vehicle = spec->name;
  b.capacityKwh = spec->capacityKwh;
  b.targetSoc = (targetSoc > 0) ? targetSoc : spec->targetSoc;
  b.soc = (soc0 > 0) ? soc0 : randRange(spec->soc0Lo, spec->soc0Hi);
  b.currentLimitA = spec->currentLimitA;
  b.priority = (priority > 0) ? priority : (uint8_t)(1 + randU32(3));
  b.faulted = false;
  b.present = true;
  b.state = ST_CHARGING;
  b.energyKwh = 0.0f;
  b.tempC = ambientC + 2.0f;
  b.predictedMin = 0.0f;      // recomputed by the Edge-AI layer on next tick
  b.timeRemainingMin = 0.0f;
  return true;
}

bool Station::plugOut(uint8_t idx) {
  if (idx < 1 || idx > 3) return false;
  Bay& b = bay[idx - 1];
  if (!b.present) return false;
  b.present = false;
  b.state = ST_IDLE;
  b.soc = 0.0f;
  b.currentA = 0.0f; b.powerKw = 0.0f; b.allowedA = 0.0f;
  b.decision = DEC_ALLOW; b.reason = "idle";
  return true;
}

void Station::simulateFault(uint8_t idx) {
  if (idx < 1 || idx > 3) return;
  bay[idx - 1].faulted = true;
  bay[idx - 1].reason = "simulated_fault";
}

void Station::clearFault(uint8_t idx) {
  if (idx < 1 || idx > 3) return;
  Bay& b = bay[idx - 1];
  b.faulted = false;
  b.reason = (b.present) ? "normal" : "idle";
}

void Station::setEnabled(uint8_t idx, bool en) {
  if (idx < 1 || idx > 3) return;
  bay[idx - 1].enabled = en;
}

// ------------------------------------------------------------------ //
void Station::runAutoEvents(float dtMin) {
  if (!autoEvents) return;
  static float nextEventMin = 6.0f;
  nextEventMin -= dtMin;
  if (nextEventMin > 0.0f) return;

  bool free[3] = {false, false, false};
  int nFree = 0;
  for (uint8_t i = 0; i < 3; i++) {
    if (!bay[i].present) { free[i] = true; nFree++; }
  }
  // probability an arrival happens within this minute = P(15 min) / 15
  float pPerMin = lastArrivalProb / 15.0f;
  if ((nFree > 0) && (randRange(0.0f, 1.0f) < pPerMin)) {
    uint8_t pick = randU32(nFree);
    for (uint8_t i = 0, k = 0; i < 3; i++) {
      if (free[i] && k++ == pick) {
        plugIn(i + 1, VEHICLES[randU32(ARRAY_SIZE(VEHICLES))].name);
        break;
      }
    }
    nextEventMin = 4.0f + randRange(0.0f, 9.0f);
  } else {
    nextEventMin = 1.5f + randRange(0.0f, 4.0f);
  }
}

// ------------------------------------------------------------------ //
const char* Station::decisionStr(uint8_t idx) const {
  switch (bay[idx - 1].decision) {
    case DEC_ALLOW: return "ALLOW";
    case DEC_THROTTLE: return "THROTTLE";
    default: return "DEFER";
  }
}

const char* Station::stateStr(uint8_t idx) const {
  switch (bay[idx - 1].state) {
    case ST_IDLE: return "IDLE";
    case ST_CHARGING: return "CHARGING";
    case ST_DONE: return "DONE";
  }
  return "?";
}

const char* Station::alarmCodes() const {
  static char buf[64];
  buf[0] = '\0';
  if (alarms & AL_TEMP_HIGH) strcat(buf, "TEMP_HIGH|");
  if (alarms & AL_TEMP_CRIT) strcat(buf, "TEMP_CRIT|");
  if (alarms & AL_OVERLOAD) strcat(buf, "OVERLOAD|");
  if (alarms & AL_VOLT_LOW) strcat(buf, "VOLTAGE|");
  if (alarms & AL_SIM_FAULT) strcat(buf, "FAULT|");
  size_t len = strlen(buf);
  if (len > 0 && buf[len - 1] == '|') buf[len - 1] = '\0';
  return buf;
}

const char* Station::simTimeString() const {
  static char buf[24];
  static const char* days[] = {"Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"};
  uint8_t h = (uint8_t)(simMinuteOfDay / 60.0f);
  uint8_t m = (uint8_t)(simMinuteOfDay - h * 60.0f);
  snprintf(buf, sizeof(buf), "%s %02u:%02u", days[simDay % 7], h, m);
  return buf;
}

int Station::activeSessions() const {
  int n = 0;
  for (uint8_t i = 0; i < 3; i++) if (bay[i].isCharging()) n++;
  return n;
}