/*
 * optimizer.cpp
 * =============
 * Implementation of the ALLOW / THROTTLE / DEFER decision engine.
 * Behavior mirrors optimizer/optimizer.py 1:1.
 */
#include "optimizer.h"
#include "ai_edge.h"

// ------------------------------------------------------------------ //
static void proportionalAllocate(Bay** running, int nRunning, float avail, float hardMax) {
  for (int iter = 0; iter < 4; iter++) {
    float requested = 0.0f;
    for (int i = 0; i < nRunning; i++) requested += running[i]->allowedA;

    if (requested <= avail + 0.01f) break;

    float scale = avail / (requested > 0.01f ? requested : 0.01f);
    for (int i = 0; i < nRunning; i++) {
      running[i]->allowedA *= scale;
      running[i]->decision = DEC_THROTTLE;
      running[i]->reason = "station_load";
    }

    // find the lowest-priority bay that fell below the useful floor
    int victim = -1;
    float worstKey = -1.0f;   // priority * 100 + (3 - index): bigger = less important
    for (int i = 0; i < nRunning; i++) {
      if (running[i]->allowedA < MIN_CHARGE_CURRENT_A) {
        float key = running[i]->priority * 100.0f + (3 - running[i]->index);
        if (key > worstKey) { worstKey = key; victim = i; }
      }
    }
    if (victim >= 0) {
      running[victim]->allowedA = 0.0f;
      running[victim]->decision = DEC_DEFER;
      running[victim]->reason = "station_load_low_priority";
      // remove the victim from the working set
      for (int j = victim; j < nRunning - 1; j++) running[j] = running[j + 1];
      nRunning--;
      avail = hardMax;   // freed head-room is reusable
    } else {
      break;
    }
  }
}

static void enforceHardLimit(Station& st) {
  for (int iter = 0; iter < 4; iter++) {
    float total = 0.0f;
    Bay* charging[3];
    int n = 0;
    for (uint8_t i = 0; i < 3; i++) {
      if (st.bay[i].isCharging()) { charging[n++] = &st.bay[i]; total += st.bay[i].allowedA; }
    }
    if (total <= st.maxStationCurrent + 0.01f) break;

    float excess = total - st.maxStationCurrent;
    for (int i = 0; i < n; i++) {
      float cut = fminf(charging[i]->allowedA, excess * charging[i]->allowedA / (total > 0.01f ? total : 0.01f));
      charging[i]->allowedA -= cut;
      if (charging[i]->allowedA < MIN_CHARGE_CURRENT_A) {
        charging[i]->allowedA = 0.0f;
        charging[i]->decision = DEC_DEFER;
        charging[i]->reason = "hard_limit";
        break;   // re-evaluate with freed head-room
      }
    }
    if (n > 0 && charging[n - 1]->allowedA < MIN_CHARGE_CURRENT_A) break;
  }
}

// ------------------------------------------------------------------ //
uint8_t runOptimizer(Station& st) {
  st.alarms &= ~(AL_TEMP_HIGH | AL_TEMP_CRIT | AL_OVERLOAD | AL_VOLT_LOW);

  // ----- safety layer (always active) -----
  for (uint8_t i = 0; i < 3; i++) {
    Bay& b = st.bay[i];
    if (!b.present) {
      b.allowedA = 0.0f; b.decision = DEC_ALLOW; b.reason = "idle";
      continue;
    }
    b.allowedA = b.currentLimitA;
    b.decision = DEC_ALLOW;
    b.reason = "normal";

    if (!b.enabled) { b.allowedA = 0; b.decision = DEC_DEFER; b.reason = "disabled"; continue; }
    if (b.faulted)  { b.allowedA = 0; b.decision = DEC_DEFER; b.reason = "fault_locked"; continue; }
    if (b.state == ST_DONE) { b.allowedA = 0; b.decision = DEC_ALLOW; b.reason = "session_complete"; continue; }
    if (b.tempC >= TEMP_CRIT_C) { b.allowedA = 0; b.decision = DEC_DEFER; b.reason = "temp_critical";
                                  st.alarms |= AL_TEMP_CRIT; continue; }
    if (b.tempC >= TEMP_HIGH_C) { b.allowedA *= 0.40f; b.decision = DEC_THROTTLE; b.reason = "temp_high";
                                  st.alarms |= AL_TEMP_HIGH; continue; }
    if (b.tempC >= TEMP_WARN_C) { b.allowedA *= 0.75f; b.decision = DEC_THROTTLE; b.reason = "temp_warm"; }
  }

  if (st.optimizerMode != 0) {          // MANUAL: safety + hard fuse only
    enforceHardLimit(st);
  } else {                              // AUTO: predictive load management
    int freeBays = 0;
    for (uint8_t i = 0; i < 3; i++) if (!st.bay[i].present) freeBays++;
    st.predictedArrivals = st.lastArrivalProb * (float)freeBays;
    float reserveA = st.predictedArrivals * 16.0f;     // ~3.7 kW per session

    int n = 0;
    Bay* running[3];
    for (uint8_t i = 0; i < 3; i++) if (st.bay[i].isCharging()) running[n++] = &st.bay[i];

    if (n > 0) {
      float requested = 0.0f;
      for (int i = 0; i < n; i++) requested += running[i]->allowedA;
      float avail = st.maxStationCurrent - reserveA;
      if (requested > avail) proportionalAllocate(running, n, avail, st.maxStationCurrent);
    }
    enforceHardLimit(st);
  }

  // ----- alarms -----
  float loadPct = 100.0f * st.stationCurrentA / (st.maxStationCurrent > 0.01f ? st.maxStationCurrent : 1.0f);
  if (loadPct >= LOAD_SOFT_LIMIT_PCT) st.alarms |= AL_OVERLOAD;
  for (uint8_t i = 0; i < 3; i++) {
    if (st.bay[i].present && st.bay[i].voltageV < VOLT_LOW_V) st.alarms |= AL_VOLT_LOW;
    if (st.bay[i].faulted) st.alarms |= AL_SIM_FAULT;
  }
  return st.alarms;
}

// ------------------------------------------------------------------ //
void refreshEdgeAI(Station& st) {
  float hour = st.simMinuteOfDay / 60.0f;
  st.lastArrivalProb = predictArrivalProbability(hour, st.simDay, st.busyRate);

  for (uint8_t i = 0; i < 3; i++) {
    Bay& b = st.bay[i];
    if (b.present && b.predictedMin <= 0.0f) {
      b.predictedMin = predictChargeMinutes(b.capacityKwh, b.soc, b.targetSoc,
                                            st.ambientC, b.currentLimitA, b.tempC);
    }
  }
}