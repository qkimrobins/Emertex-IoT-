/*
 * display.cpp
 * ===========
 * OLED rendering for the station summary and per-bay views.
 */
#include "display.h"

StationDisplay display;

// ------------------------------------------------------------------ //
void StationDisplay::begin() {
  Wire.begin(PIN_OLED_SDA, PIN_OLED_SCL);
  if (!oled_.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
    Serial.println("[display] SSD1306 init failed (continuing without)");
  }
  oled_.clearDisplay();
  oled_.setTextSize(1);
  oled_.setTextColor(SSD1306_WHITE);
  oled_.setCursor(0, 0);
  oled_.print("Smart EV Station");
  oled_.display();
}

// ------------------------------------------------------------------ //
void StationDisplay::render(Station& st, bool tbOk, bool mirrorOk) {
  unsigned long now = millis();
  if (now - lastMs_ < 2500UL) return;
  lastMs_ = now;
  secondScreen_ = !secondScreen_;

  oled_.clearDisplay();
  if (secondScreen_) drawBays(st);
  else drawSummary(st, tbOk, mirrorOk);
  oled_.display();
}

// ------------------------------------------------------------------ //
void StationDisplay::drawSummary(Station& st, bool tbOk, bool mirrorOk) {
  oled_.setCursor(0, 0);
  oled_.println("== Smart EV Station ==");
  oled_.printf("Load %4.1f/%3.0fA %3.0f%%\n", st.stationCurrentA,
               st.maxStationCurrent,
               100.0f * st.stationCurrentA / st.maxStationCurrent);
  oled_.printf("P %5.2f kW  E %5.2f kWh\n", st.stationPowerKw, st.stationEnergyKwh);
  oled_.printf("AI p=%3.0f%% sess=%d\n", st.lastArrivalProb * 100.0f,
               (int)st.predictedArrivals);
  oled_.printf("Mode %s\n", (st.optimizerMode == 0) ? "AUTO" : "MANUAL");
  oled_.printf("Alarm: %s\n", (st.alarms == 0) ? "none" : st.alarmCodes());
  oled_.printf("TB:%s MR:%s\n", tbOk ? "ok" : "XX", mirrorOk ? "ok" : "XX");
}

// ------------------------------------------------------------------ //
void StationDisplay::drawBays(Station& st) {
  for (uint8_t i = 0; i < 3; i++) {
    Bay& b = st.bay[i];
    int y = i * 21;
    oled_.setCursor(0, y);
    oled_.printf("B%d %-11s", b.index, b.present ? b.vehicle : "(free)");
    oled_.drawRect(88, y, 40, 7, SSD1306_WHITE);
    if (b.present) drawSocBar(89, y + 1, 38, 5, b.soc);
    oled_.setCursor(0, y + 8);
    oled_.printf(" %5.1fA %3.0f%% %s %s", b.currentA, b.soc,
                 st.decisionStr(b.index), b.present ? b.reason : "");
  }
}

void StationDisplay::drawSocBar(int x, int y, int w, int h, float soc) {
  int fill = (int)(w * CONSTRAIN(soc / 100.0f, 0.0f, 1.0f));
  if (fill > 0) oled_.fillRect(x, y, fill, h, SSD1306_WHITE);
}