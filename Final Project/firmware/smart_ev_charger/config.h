/*
 * config.h
 * ========
 * Central configuration for the Smart Edge-AI EV Charging Station Optimizer
 * firmware. Edit the credentials / broker settings here before flashing.
 *
 * The firmware maintains TWO MQTT connections on purpose:
 *
 *   TB      -> ThingsBoard (telemetry on v1/devices/me/telemetry, RPC + shared
 *              attributes per the ThingsBoard MQTT device API)
 *   MIRROR  -> a public broker that the web dashboard listens on. This keeps
 *              the browser demo working even when ThingsBoard is not running.
 */
#pragma once

// ------------------------------------------------------------------ //
// WiFi (Wokwi simulator network is "Wokwi-GUEST" with no password)
// ------------------------------------------------------------------ //
#define WIFI_SSID "Wokwi-GUEST"
#define WIFI_PASS ""

// ------------------------------------------------------------------ //
// ThingsBoard MQTT (device credentials)
// ------------------------------------------------------------------ //
#define TB_ENABLED 1
#define TB_HOST "demo.thingsboard.io"      // or your own ThingsBoard CE host
#define TB_PORT 1883
#define TB_ACCESS_TOKEN "JnU9NDcyMzIyNjc5OTI1MTE3OTUzJm49Um9iaW5zK1lhZGF2JmU9c3NjbnJvYmlucyU0MGdtYWlsLmNvbSZ4PTIwMjYxMDE5ABYqrd4GgpZ75oWw49nHi5M5D2o1FKcSJyu2ZbDH5aG0eWn97JZ4NXj8gVE7DPmZwpP5OTa064mDuRw_Pp38BJi1BmVok9fJ9CMVe6_PG28QmkHKpVfFwuqgW6Ymt4N1uhPLeqayUe6mYz2sDXaqQzXsw28_PAoYzRcKQTiPqCCQxfNT1_SlUkmf9E_Scr5o5NoANXQYaBFN8T6_SY3c2OAkDaeJfzkrO_PojOb5o_SqP3u8XhPA5DR_P32Rin0kpSYDkqP616Nw9qY73r5uIAUvvJOsfVWt14tY_PVQp9SwETJklatOCnEz1MUm_SNPiVg_PPGGr4GKne0cXgq8Ji3TG1HNAvGx1kw"

// ------------------------------------------------------------------ //
// Public "mirror" broker for the web dashboard (MQTT.js in the browser)
// ------------------------------------------------------------------ //
#define MIRROR_HOST "broker.emqx.io"
#define MIRROR_PORT 1883

// ------------------------------------------------------------------ //
// MQTT topics
// ------------------------------------------------------------------ //
#define TOPIC_TB_TELEMETRY "v1/devices/me/telemetry"
#define TOPIC_TB_ATTRIBUTES "v1/devices/me/attributes"
#define TOPIC_TB_RPC_REQ "v1/devices/me/rpc/request/+"
#define TOPIC_TB_RPC_RESP "v1/devices/me/rpc/response/"   // + requestId
#define TOPIC_MIRROR_TELEMETRY "emx/ev/telemetry"
#define TOPIC_MIRROR_COMMAND "emx/ev/cmd"
#define TOPIC_MIRROR_ACK "emx/ev/cmd/ack"
#define TOPIC_MIRROR_ATTR_STATE "emx/ev/attr/state"
#define TOPIC_MIRROR_STATUS "emx/ev/status"

// ------------------------------------------------------------------ //
// Electrical ratings + safety thresholds (shared with optimizer/)
// ------------------------------------------------------------------ //
#define STATION_MAX_CURRENT_DEFAULT 64.0f   // A (feeder / transformer rating)
#define BAY_CURRENT_LIMIT_DEFAULT 32.0f     // A per socket (7.4 kW @ 230 V)
#define MIN_CHARGE_CURRENT_A 6.0f           // below this the session is paused
#define V_NOM 230.0f                        // nominal mains voltage (V)
#define PF 0.98f                            // power factor
#define ETA 0.92f                           // on-board charger efficiency
#define TEMP_WARN_C 55.0f                   // -> THROTTLE to 75 %
#define TEMP_HIGH_C 65.0f                   // -> THROTTLE to 40 %
#define TEMP_CRIT_C 80.0f                   // -> DEFER + critical alarm
#define VOLT_LOW_V 210.0f                   // undervoltage alarm level
#define LOAD_SOFT_LIMIT_PCT 90.0f           // station-load alarm level

// ------------------------------------------------------------------ //
// Simulation
// ------------------------------------------------------------------ //
#define SIM_TIME_ACCEL 60                   // sim minutes per real second
#define SIM_DAY_START 2                     // 0=Monday ... 6=Sunday (demo clock)
#define SIM_MINUTE_START (8.0f * 60.0f)     // demo clock starts at 08:00

// ------------------------------------------------------------------ //
// Control / timing (ms)
// ------------------------------------------------------------------ //
#define TICK_MS 1000UL                      // simulation tick
#define TELEMETRY_EVERY_MS 5000UL           // publish telemetry every 5 s
#define OPTIMIZER_EVERY_MS 3000UL           // recalculate decisions every 3 s
#define AI_EVERY_MS 15000UL                 // refresh Edge-AI every 15 s

// ------------------------------------------------------------------ //
// Pins
// ------------------------------------------------------------------ //
#define PIN_OLED_SDA 21
#define PIN_OLED_SCL 22
#define PIN_LED_BUILTIN 2
#define PIN_LED_BAY1 26
#define PIN_LED_BAY2 27
#define PIN_LED_BAY3 14
#define PIN_LED_ALARM 13

// ------------------------------------------------------------------ //
// Small helpers
// ------------------------------------------------------------------ //
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define CONSTRAIN(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))