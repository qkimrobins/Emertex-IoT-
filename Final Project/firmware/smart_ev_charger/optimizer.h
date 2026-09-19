/*
 * optimizer.h
 * ===========
 * Load-management optimizer: turns the raw electrical telemetry + Edge-AI
 * predictions into one ALLOW / THROTTLE / DEFER decision per bay.

 * Decision policy (mirrors optimizer/optimizer.py):
 *   1. SAFETY layer (always active):
 *        - disabled socket / fault lock      -> DEFER
 *        - temp >= 80 C (critical)           -> DEFER + alarm
 *        - temp >= 65 C                      -> THROTTLE to 40 %
 *        - temp >= 55 C                      -> THROTTLE to 75 %
 *   2. LOAD layer (AUTO mode only):
 *        - reserve head-room for Edge-AI predicted arrivals
 *        - if the station would exceed its rating, throttle proportionally,
 *          deferring the lowest-priority bay that drops under 6 A
 *   3. HARD fuse (always): station current may never exceed the rating.
 */
#pragma once

#include <Arduino.h>
#include "ev_station.h"
#include "config.h"

// Recompute the per-bay decisions. Returns the station alarm bitmask.
uint8_t runOptimizer(Station& st);

// Refresh the Edge-AI predictions (arrival probability + session duration).
void refreshEdgeAI(Station& st);