/*
 * display.h
 * =========
 * SSD1306 OLED (128x64) status screen for the Wokwi demo.
 * Rotates between the station summary and the per-bay view so a viva audience
 * can read the station state directly off the "hardware".
 */
#pragma once

#include <Arduino.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#include "config.h"
#include "ev_station.h"

class StationDisplay {
 public:
  void begin();
  void render(Station& st, bool tbOk, bool mirrorOk);

 private:
  Adafruit_SSD1306 oled_ = Adafruit_SSD1306(128, 64, &Wire, -1);
  unsigned long lastMs_ = 0;
  bool secondScreen_ = false;

  void drawSummary(Station& st, bool tbOk, bool mirrorOk);
  void drawBays(Station& st);
  void drawSocBar(int x, int y, int w, int h, float soc);
};

extern StationDisplay display;