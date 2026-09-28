// =====================================================================
//   T-EMBED LAB — мультитул-прошивка для LILYGO T-Embed (ESP32-S3)
//   Меню на энкодере + набор легальных модулей.
//
//   Модули: Система, Датчик среды (BME280), Часы (интернет),
//           Секундомер, Сканер Wi-Fi, RGB-подсветка, Фонарик, Настройки.
//
//   Управление одним энкодером:
//     - Крутить            → перемещение / изменение значения
//     - Короткое нажатие   → выбрать / действие на экране
//     - Долгое нажатие(>0.6с) → назад в меню
//
//   БИБЛИОТЕКИ (Arduino IDE → Менеджер библиотек):
//     - "GFX Library for Arduino"  (автор moononournation)   — обязательно
//     - "Adafruit BME280 Library" + "Adafruit Unified Sensor" — только если
//        ENABLE_BME280 = 1 (см. ниже). Нет датчика — поставь 0, и всё скомпилируется.
//
//   НАСТРОЙКИ ПЛАТЫ (меню Tools):
//     Board: "ESP32S3 Dev Module"
//     USB CDC On Boot: "Enabled"
//     PSRAM: "OPI PSRAM"
//     Flash Size: 16MB   (Partition Scheme на твоё усмотрение)
//
//   Файл pin_config.h должен лежать рядом с этим .ino в папке TEmbedLab.
// =====================================================================

#include <Arduino_GFX_Library.h>
#include <WiFi.h>
#include <Wire.h>
#include <time.h>
#include "pin_config.h"

// ---------------------------------------------------------------------
//  ПОЛЬЗОВАТЕЛЬСКИЕ НАСТРОЙКИ
// ---------------------------------------------------------------------
#define ENABLE_BME280   1              // 0 = отключить датчик и не требовать его библиотеку

#define WIFI_SSID       "ТВОЯ_СЕТЬ"    // впиши имя своей Wi-Fi сети
#define WIFI_PASS       "ТВОЙ_ПАРОЛЬ"  // и пароль (нужно для часов/интернета)

// Часовой пояс. Москва (UTC+3, без перехода на летнее время):
#define TZ_STRING       "MSK-3"
// Примеры для других поясов: Хельсинки "EET-2EEST,M3.5.0/3,M10.5.0/4"
#define NTP_SERVER      "pool.ntp.org"

#define NUM_LEDS        7              // сколько APA102 на плате (лишние кадры безвредны)

// ---------------------------------------------------------------------
//  ДИСПЛЕЙ (Arduino_GFX, настройка прямо в скетче — библиотеку править не нужно)
// ---------------------------------------------------------------------
Arduino_DataBus *bus = new Arduino_ESP32SPI(
    PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_CLK, PIN_LCD_MOSI, GFX_NOT_DEFINED);

// 170x320 IPS. rotation=1 → альбомная 320x170. Если картинка сдвинута/
// перевёрнута — поменяй rotation (0..3) или смещения 35/0.
Arduino_GFX *gfx = new Arduino_ST7789(
    bus, PIN_LCD_RES, 1 /*rotation*/, true /*IPS*/,
    170, 320, 35, 0, 35, 0);

int SCR_W = 320;   // фактические размеры после поворота (заполнятся в setup)
int SCR_H = 170;

// ---------------------------------------------------------------------
//  Конвертация цвета R,G,B (0..255) в 16-бит 565 — в Arduino_GFX нет
//  метода color565(), поэтому используем свою функцию.
// ---------------------------------------------------------------------
static inline uint16_t C565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

// ---------------------------------------------------------------------
//  ЦВЕТОВАЯ ТЕМА
// ---------------------------------------------------------------------
uint16_t C_BG, C_PANEL, C_ACCENT, C_ACCENT2, C_TEXT, C_MUTED, C_WARN, C_OK;

void initTheme() {
  C_BG      = C565(9, 12, 18);      // почти чёрный фон
  C_PANEL   = C565(22, 27, 38);     // панель/плитка
  C_ACCENT  = C565(60, 223, 138);   // портально-зелёный
  C_ACCENT2 = C565(120, 180, 255);  // голубой
  C_TEXT    = C565(232, 238, 245);  // белый
  C_MUTED   = C565(120, 132, 150);  // серый
  C_WARN    = C565(255, 96, 96);    // красный
  C_OK      = C565(80, 220, 120);   // зелёный
}

// ---------------------------------------------------------------------
//  ЭКРАНЫ
// ---------------------------------------------------------------------
enum Screen {
  SCR_MENU, SCR_SYSINFO, SCR_ENV, SCR_CLOCK, SCR_STOPWATCH,
  SCR_WIFI, SCR_LED, SCR_LIGHT, SCR_SETTINGS
};
Screen screen = SCR_MENU;
bool   screenEntered = true;   // true → нужно отрисовать экран целиком

struct MenuItem { const char *name; Screen scr; uint8_t icon; };
MenuItem menu[] = {
  {"Система",   SCR_SYSINFO,   0},
  {"Среда",     SCR_ENV,       1},
  {"Часы",      SCR_CLOCK,     2},
  {"Секундомер",SCR_STOPWATCH, 3},
  {"Wi-Fi",     SCR_WIFI,      4},
  {"Подсветка", SCR_LED,       5},
  {"Фонарик",   SCR_LIGHT,     6},
  {"Настройки", SCR_SETTINGS,  7},
};
const int MENU_N = sizeof(menu) / sizeof(menu[0]);
int menuSel = 0;

// ---------------------------------------------------------------------
//  ЭНКОДЕР + КНОПКА (опрос, без прерываний — надёжно для меню)
// ---------------------------------------------------------------------
const int8_t ENC_TABLE[16] = {0,-1,1,0, 1,0,0,-1, -1,0,0,1, 0,1,-1,0};
uint8_t  encState = 0;
int      encAccum = 0;
int      encoderDelta = 0;   // готовые «щелчки» за текущий проход loop()

enum BtnEvent { BTN_NONE, BTN_SHORT, BTN_LONG };
BtnEvent btnEvent = BTN_NONE;
bool     btnDown = false;
uint32_t btnDownTime = 0;
bool     btnLongFired = false;

void inputBegin() {
  pinMode(PIN_ENCODE_A, INPUT_PULLUP);
  pinMode(PIN_ENCODE_B, INPUT_PULLUP);
  pinMode(PIN_ENCODE_BTN, INPUT_PULLUP);
  encState = (digitalRead(PIN_ENCODE_A) << 1) | digitalRead(PIN_ENCODE_B);
}

void inputPoll() {
  encoderDelta = 0;
  btnEvent = BTN_NONE;

  // --- энкодер ---
  uint8_t s = (digitalRead(PIN_ENCODE_A) << 1) | digitalRead(PIN_ENCODE_B);
  encAccum += ENC_TABLE[(encState << 2) | s];
  encState = s;
  if (encAccum >= 4)  { encoderDelta = 1;  encAccum = 0; }
  if (encAccum <= -4) { encoderDelta = -1; encAccum = 0; }

  // --- кнопка ---
  bool pressed = (digitalRead(PIN_ENCODE_BTN) == LOW);
  uint32_t now = millis();
  if (pressed && !btnDown) {                 // нажали
    btnDown = true; btnDownTime = now; btnLongFired = false;
  } else if (pressed && btnDown) {           // держим
    if (!btnLongFired && now - btnDownTime > 600) {
      btnEvent = BTN_LONG; btnLongFired = true;
    }
  } else if (!pressed && btnDown) {          // отпустили
    btnDown = false;
    if (!btnLongFired && now - btnDownTime > 25) btnEvent = BTN_SHORT;
  }
}

// ---------------------------------------------------------------------
//  RGB-ПОДСВЕТКА APA102 (bit-bang, без библиотеки)
// ---------------------------------------------------------------------
void apaByte(uint8_t b) {
  for (int i = 0; i < 8; i++) {
    digitalWrite(PIN_APA102_DI, (b & 0x80) ? HIGH : LOW);
    b <<= 1;
    digitalWrite(PIN_APA102_CLK, HIGH);
    digitalWrite(PIN_APA102_CLK, LOW);
  }
}
void apaBegin() {
  pinMode(PIN_APA102_CLK, OUTPUT);
  pinMode(PIN_APA102_DI, OUTPUT);
}
// bright: 0..31, цвет 0..255
void apaShow(uint8_t r, uint8_t g, uint8_t b, uint8_t bright) {
  apaByte(0); apaByte(0); apaByte(0); apaByte(0);           // start frame
  for (int i = 0; i < NUM_LEDS; i++) {
    apaByte(0xE0 | (bright & 0x1F));                        // 111 + яркость
    apaByte(b); apaByte(g); apaByte(r);                     // порядок B,G,R
  }
  apaByte(0xFF); apaByte(0xFF); apaByte(0xFF); apaByte(0xFF); // end frame
}

// ---------------------------------------------------------------------
//  ПОДСВЕТКА ЭКРАНА (яркость через ШИМ, совместимо с core 2.x и 3.x)
// ---------------------------------------------------------------------
uint8_t g_backlight = 210;   // 0..255
void backlightBegin() {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcAttach(PIN_LCD_BL, 5000, 8);
#else
  ledcSetup(0, 5000, 8);
  ledcAttachPin(PIN_LCD_BL, 0);
#endif
}
void backlightSet(uint8_t v) {
  g_backlight = v;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcWrite(PIN_LCD_BL, v);
#else
  ledcWrite(0, v);
#endif
}

// ---------------------------------------------------------------------
//  АККУМУЛЯТОР
// ---------------------------------------------------------------------
float readBatteryV() {
  uint32_t mv = analogReadMilliVolts(PIN_BAT_VOLT);
  return (mv * 2.0f) / 1000.0f;   // делитель 1:2
}
int batteryPercent(float v) {
  int p = (int)((v - 3.30f) / (4.20f - 3.30f) * 100.0f);
  return constrain(p, 0, 100);
}

// ---------------------------------------------------------------------
//  ДАТЧИК СРЕДЫ BME280 (опционально)
// ---------------------------------------------------------------------
#if ENABLE_BME280
#include <Adafruit_BME280.h>
Adafruit_BME280 bme;
bool bmeOk = false;
void bmeBegin() {
  // адрес 0x76 (частый) или 0x77
  bmeOk = bme.begin(0x76, &Wire) || bme.begin(0x77, &Wire);
}
#endif

// ---------------------------------------------------------------------
//  ВРЕМЯ / NTP
// ---------------------------------------------------------------------
bool wifiTried = false;
bool timeSynced = false;

bool wifiConnect(uint32_t timeoutMs = 8000) {
  if (WiFi.status() == WL_CONNECTED) return true;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < timeoutMs) delay(150);
  return WiFi.status() == WL_CONNECTED;
}
void timeSync() {
  if (!wifiConnect()) return;
  configTzTime(TZ_STRING, NTP_SERVER);
  struct tm ti;
  if (getLocalTime(&ti, 6000)) timeSynced = true;
}
bool getClock(struct tm &ti) {
  return timeSynced && getLocalTime(&ti, 50);
}

// =====================================================================
//  ГРАФИЧЕСКИЕ ХЕЛПЕРЫ
// =====================================================================
void textAt(int x, int y, const char *s, uint16_t color, uint8_t size = 1) {
  gfx->setTextColor(color);
  gfx->setTextSize(size);
  gfx->setCursor(x, y);
  gfx->print(s);
}

// маленькая иконка батареи + процент справа сверху
void drawBattery(int x, int y) {
  float v = readBatteryV();
  int pct = batteryPercent(v);
  uint16_t col = pct > 20 ? C_OK : C_WARN;
  gfx->drawRoundRect(x, y, 26, 12, 2, C_MUTED);
  gfx->fillRect(x + 26, y + 3, 2, 6, C_MUTED);
  int w = map(pct, 0, 100, 0, 22);
  gfx->fillRoundRect(x + 2, y + 2, w, 8, 1, col);
  char buf[8];
  snprintf(buf, sizeof(buf), "%d%%", pct);
  textAt(x - 34, y + 2, buf, C_MUTED, 1);
}

// верхняя панель с заголовком, временем и батареей
void drawHeader(const char *title) {
  gfx->fillRect(0, 0, SCR_W, 24, C_PANEL);
  gfx->fillRect(0, 24, SCR_W, 2, C_ACCENT);
  textAt(10, 8, title, C_TEXT, 2);

  struct tm ti;
  if (getClock(ti)) {
    char t[8];
    snprintf(t, sizeof(t), "%02d:%02d", ti.tm_hour, ti.tm_min);
    textAt(SCR_W - 160, 8, t, C_ACCENT, 2);
  }
  drawBattery(SCR_W - 30, 6);
}

// подсказка по управлению внизу
void drawFooter(const char *hint) {
  gfx->fillRect(0, SCR_H - 16, SCR_W, 16, C_PANEL);
  textAt(8, SCR_H - 13, hint, C_MUTED, 1);
}

// =====================================================================
//  ИКОНКИ ДЛЯ ПЛИТОК МЕНЮ (рисуем примитивами)
// =====================================================================
void drawIcon(uint8_t id, int cx, int cy, uint16_t col) {
  switch (id) {
    case 0: // чип
      gfx->drawRect(cx - 8, cy - 8, 16, 16, col);
      gfx->fillRect(cx - 3, cy - 3, 6, 6, col);
      for (int i = -6; i <= 6; i += 4) {
        gfx->drawFastVLine(cx + i, cy - 12, 4, col);
        gfx->drawFastVLine(cx + i, cy + 9, 4, col);
        gfx->drawFastHLine(cx - 12, cy + i, 4, col);
        gfx->drawFastHLine(cx + 9, cy + i, 4, col);
      }
      break;
    case 1: // термометр
      gfx->drawCircle(cx, cy + 7, 4, col);
      gfx->fillCircle(cx, cy + 7, 2, col);
      gfx->drawFastVLine(cx - 2, cy - 9, 14, col);
      gfx->drawFastVLine(cx + 2, cy - 9, 14, col);
      gfx->drawFastHLine(cx - 2, cy - 9, 4, col);
      break;
    case 2: // часы
      gfx->drawCircle(cx, cy, 10, col);
      gfx->drawLine(cx, cy, cx, cy - 6, col);
      gfx->drawLine(cx, cy, cx + 4, cy + 2, col);
      break;
    case 3: // секундомер
      gfx->drawCircle(cx, cy + 1, 9, col);
      gfx->drawFastVLine(cx, cy - 12, 4, col);
      gfx->drawLine(cx, cy + 1, cx + 3, cy - 4, col);
      break;
    case 4: // wi-fi
      for (int r = 4; r <= 12; r += 4)
        gfx->drawCircleHelper(cx, cy + 6, r, 0x03, col); // верхние дуги
      gfx->fillCircle(cx, cy + 6, 2, col);
      break;
    case 5: // подсветка (лампочка-RGB)
      gfx->fillCircle(cx, cy - 2, 7, col);
      gfx->fillRect(cx - 3, cy + 5, 6, 4, col);
      break;
    case 6: // фонарик
      gfx->fillTriangle(cx - 6, cy - 8, cx + 6, cy - 8, cx, cy + 2, col);
      gfx->fillRect(cx - 3, cy + 2, 6, 7, col);
      break;
    case 7: // шестерёнка
      gfx->drawCircle(cx, cy, 7, col);
      gfx->fillCircle(cx, cy, 2, col);
      for (int a = 0; a < 360; a += 45) {
        float rad = a * 3.14159 / 180.0;
        int x1 = cx + cos(rad) * 7,  y1 = cy + sin(rad) * 7;
        int x2 = cx + cos(rad) * 11, y2 = cy + sin(rad) * 11;
        gfx->drawLine(x1, y1, x2, y2, col);
      }
      break;
  }
}

// =====================================================================
//  ЭКРАН: ГЛАВНОЕ МЕНЮ (плитки 4x2)
// =====================================================================
void drawMenuTile(int i, bool sel) {
  int col = i % 4, row = i / 4;
  int tw = 74, th = 56, gx = 6, gy = 8;
  int x = 8 + col * (tw + gx);
  int y = 30 + row * (th + gy);
  uint16_t border = sel ? C_ACCENT : C_PANEL;
  uint16_t fill   = sel ? C565(30, 46, 40) : C_PANEL;
  gfx->fillRoundRect(x, y, tw, th, 8, fill);
  gfx->drawRoundRect(x, y, tw, th, 8, border);
  drawIcon(menu[i].icon, x + tw / 2, y + 20, sel ? C_ACCENT : C_TEXT);
  gfx->setTextSize(1);
  int lw = strlen(menu[i].name) * 6;
  textAt(x + (tw - lw) / 2, y + th - 14, menu[i].name, sel ? C_TEXT : C_MUTED, 1);
}
void drawMenu(bool full) {
  if (full) {
    gfx->fillScreen(C_BG);
    drawHeader("T-EMBED LAB");
    for (int i = 0; i < MENU_N; i++) drawMenuTile(i, i == menuSel);
    drawFooter("Крути: выбор   Клик: открыть");
  }
}
void screenMenu() {
  if (screenEntered) { drawMenu(true); screenEntered = false; }
  if (encoderDelta) {
    int prev = menuSel;
    menuSel = (menuSel + encoderDelta + MENU_N) % MENU_N;
    drawMenuTile(prev, false);
    drawMenuTile(menuSel, true);
  }
  if (btnEvent == BTN_SHORT) { screen = menu[menuSel].scr; screenEntered = true; }
}

// =====================================================================
//  ЭКРАН: СИСТЕМА
// =====================================================================
void screenSysInfo() {
  static uint32_t last = 0;
  if (screenEntered) {
    gfx->fillScreen(C_BG);
    drawHeader("Система");
    drawFooter("Долгое нажатие: назад");
    screenEntered = false; last = 0;
  }
  if (millis() - last < 1000) return;
  last = millis();
  gfx->fillRect(0, 28, SCR_W, SCR_H - 44, C_BG);
  int y = 34; char b[48];

  snprintf(b, sizeof(b), "Чип: %s  rev %d", ESP.getChipModel(), ESP.getChipRevision());
  textAt(12, y, b, C_TEXT, 1); y += 16;
  snprintf(b, sizeof(b), "Ядер: %d   %d МГц", ESP.getChipCores(), getCpuFrequencyMhz());
  textAt(12, y, b, C_TEXT, 1); y += 16;
  snprintf(b, sizeof(b), "RAM своб.: %u КБ", ESP.getFreeHeap() / 1024);
  textAt(12, y, b, C_TEXT, 1); y += 16;
  snprintf(b, sizeof(b), "PSRAM: %u КБ", ESP.getPsramSize() / 1024);
  textAt(12, y, b, C_TEXT, 1); y += 16;
  snprintf(b, sizeof(b), "Flash: %u МБ", ESP.getFlashChipSize() / (1024 * 1024));
  textAt(12, y, b, C_TEXT, 1); y += 16;
  float v = readBatteryV();
  snprintf(b, sizeof(b), "Батарея: %.2f В (%d%%)", v, batteryPercent(v));
  textAt(12, y, b, C_ACCENT, 1); y += 16;
  uint32_t up = millis() / 1000;
  snprintf(b, sizeof(b), "Аптайм: %02u:%02u:%02u", up / 3600, (up % 3600) / 60, up % 60);
  textAt(12, y, b, C_MUTED, 1);
}

// =====================================================================
//  ЭКРАН: СРЕДА (BME280) с бегущим графиком температуры
// =====================================================================
#define GRAPH_N 120
float gTemp[GRAPH_N];
int   gCount = 0;
void screenEnv() {
  static uint32_t last = 0;
  if (screenEntered) {
    gfx->fillScreen(C_BG);
    drawHeader("Среда");
    drawFooter("Долгое нажатие: назад");
    screenEntered = false; last = 0; gCount = 0;
  }
  if (millis() - last < 1000) return;
  last = millis();
  gfx->fillRect(0, 28, SCR_W, SCR_H - 44, C_BG);

#if ENABLE_BME280
  if (!bmeOk) {
    textAt(12, 70, "Датчик BME280 не найден.", C_WARN, 1);
    textAt(12, 88, "Проверь подключение к SDA/SCL", C_MUTED, 1);
    return;
  }
  float t = bme.readTemperature();
  float h = bme.readHumidity();
  float p = bme.readPressure() / 100.0f;   // гПа

  char b[32];
  snprintf(b, sizeof(b), "%.1f C", t);
  textAt(12, 34, b, C_ACCENT, 3);
  snprintf(b, sizeof(b), "Влажность: %.0f %%", h);
  textAt(12, 66, b, C_TEXT, 1);
  snprintf(b, sizeof(b), "Давление: %.0f гПа", p);
  textAt(12, 82, b, C_TEXT, 1);

  // история температуры
  if (gCount < GRAPH_N) gTemp[gCount++] = t;
  else { for (int i = 1; i < GRAPH_N; i++) gTemp[i - 1] = gTemp[i]; gTemp[GRAPH_N - 1] = t; }

  // график
  int gx = 12, gy = 100, gw = SCR_W - 24, gh = 44;
  gfx->drawRect(gx, gy, gw, gh, C_PANEL);
  float mn = 1e9, mx = -1e9;
  for (int i = 0; i < gCount; i++) { mn = min(mn, gTemp[i]); mx = max(mx, gTemp[i]); }
  if (mx - mn < 1) { mx += 0.5; mn -= 0.5; }
  for (int i = 1; i < gCount; i++) {
    int x1 = gx + (i - 1) * gw / GRAPH_N;
    int x2 = gx + i * gw / GRAPH_N;
    int y1 = gy + gh - (int)((gTemp[i - 1] - mn) / (mx - mn) * (gh - 4)) - 2;
    int y2 = gy + gh - (int)((gTemp[i]     - mn) / (mx - mn) * (gh - 4)) - 2;
    gfx->drawLine(x1, y1, x2, y2, C_ACCENT);
  }
#else
  textAt(12, 70, "Модуль BME280 выключен", C_WARN, 1);
  textAt(12, 88, "(ENABLE_BME280 = 0)", C_MUTED, 1);
#endif
}

// =====================================================================
//  ЭКРАН: ЧАСЫ (крупное время по интернету)
// =====================================================================
void screenClock() {
  static uint32_t last = 0;
  if (screenEntered) {
    gfx->fillScreen(C_BG);
    drawHeader("Часы");
    drawFooter("Клик: синхронизировать   Долгое: назад");
    screenEntered = false; last = 0;
    if (!timeSynced) { textAt(60, 80, "Синхронизация...", C_MUTED, 1); timeSync(); }
  }
  if (btnEvent == BTN_SHORT) { timeSynced = false; timeSync(); }
  if (millis() - last < 500) return;
  last = millis();
  gfx->fillRect(0, 28, SCR_W, SCR_H - 44, C_BG);

  struct tm ti;
  if (getClock(ti)) {
    char t[16], d[32];
    snprintf(t, sizeof(t), "%02d:%02d:%02d", ti.tm_hour, ti.tm_min, ti.tm_sec);
    textAt(28, 60, t, C_ACCENT, 4);
    snprintf(d, sizeof(d), "%02d.%02d.%04d", ti.tm_mday, ti.tm_mon + 1, ti.tm_year + 1900);
    textAt(90, 110, d, C_TEXT, 2);
  } else {
    textAt(40, 80, "Нет времени. Впиши Wi-Fi", C_WARN, 1);
    textAt(40, 96, "в настройках прошивки", C_MUTED, 1);
  }
}

// =====================================================================
//  ЭКРАН: СЕКУНДОМЕР
// =====================================================================
bool     swRun = false;
uint32_t swStart = 0, swElapsed = 0;
void screenStopwatch() {
  static uint32_t last = 0;
  if (screenEntered) {
    gfx->fillScreen(C_BG);
    drawHeader("Секундомер");
    drawFooter("Клик: старт/стоп   Долгое: назад(=сброс)");
    screenEntered = false; last = 0;
  }
  if (btnEvent == BTN_SHORT) {
    if (swRun) { swElapsed += millis() - swStart; swRun = false; }
    else       { swStart = millis(); swRun = true; }
  }
  if (millis() - last < 50) return;
  last = millis();
  uint32_t ms = swElapsed + (swRun ? millis() - swStart : 0);
  int m = ms / 60000, s = (ms / 1000) % 60, cs = (ms % 1000) / 10;
  char t[16];
  snprintf(t, sizeof(t), "%02d:%02d.%02d", m, s, cs);
  gfx->fillRect(0, 55, SCR_W, 50, C_BG);
  textAt(30, 60, t, swRun ? C_ACCENT : C_TEXT, 4);
}

// =====================================================================
//  ЭКРАН: СКАНЕР Wi-Fi (только просмотр эфира — легально)
// =====================================================================
int    wifiN = -1;
void screenWifi() {
  if (screenEntered) {
    gfx->fillScreen(C_BG);
    drawHeader("Wi-Fi");
    drawFooter("Клик: пересканировать   Долгое: назад");
    textAt(12, 40, "Сканирую сети...", C_MUTED, 1);
    screenEntered = false;
    WiFi.mode(WIFI_STA);
    wifiN = WiFi.scanNetworks();
  }
  if (btnEvent == BTN_SHORT) {
    gfx->fillRect(0, 28, SCR_W, SCR_H - 44, C_BG);
    textAt(12, 40, "Сканирую сети...", C_MUTED, 1);
    wifiN = WiFi.scanNetworks();
  }
  static int shownFor = -999;
  if (wifiN == shownFor) return;
  shownFor = wifiN;

  gfx->fillRect(0, 28, SCR_W, SCR_H - 44, C_BG);
  char b[64];
  snprintf(b, sizeof(b), "Найдено сетей: %d", wifiN);
  textAt(12, 32, b, C_ACCENT, 1);
  int y = 50;
  int lim = min(wifiN, 6);
  for (int i = 0; i < lim; i++) {
    int rssi = WiFi.RSSI(i);
    int bars = map(constrain(rssi, -90, -40), -90, -40, 1, 4);
    uint16_t col = rssi > -67 ? C_OK : (rssi > -80 ? C_ACCENT2 : C_MUTED);
    for (int bIdx = 0; bIdx < 4; bIdx++)
      gfx->fillRect(12 + bIdx * 4, y + 8 - bIdx * 2, 3, 2 + bIdx * 2,
                    bIdx < bars ? col : C_PANEL);
    String ssid = WiFi.SSID(i);
    if (ssid.length() == 0) ssid = "(скрытая)";
    if (ssid.length() > 20) ssid = ssid.substring(0, 20);
    snprintf(b, sizeof(b), "%s  ch%d %ddBm",
             ssid.c_str(), WiFi.channel(i), rssi);
    textAt(34, y, b, C_TEXT, 1);
    y += 18;
  }
}

// =====================================================================
//  ЭКРАН: RGB-ПОДСВЕТКА (крутим — меняем оттенок)
// =====================================================================
int ledHue = 100;   // 0..255
void hsv2rgb(uint8_t h, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t region = h / 43, rem = (h - region * 43) * 6;
  uint8_t p = 0, q = 255 - ((255 * rem) >> 8), t = (255 * rem) >> 8;
  switch (region) {
    case 0: r = 255; g = t; b = p; break;
    case 1: r = q; g = 255; b = p; break;
    case 2: r = p; g = 255; b = t; break;
    case 3: r = p; g = q; b = 255; break;
    case 4: r = t; g = p; b = 255; break;
    default: r = 255; g = p; b = q; break;
  }
}
void screenLed() {
  if (screenEntered) {
    gfx->fillScreen(C_BG);
    drawHeader("Подсветка");
    drawFooter("Крути: цвет   Долгое: назад");
    screenEntered = false; encoderDelta = 999; // форсируем первую отрисовку
  }
  if (encoderDelta) {
    if (encoderDelta != 999) ledHue = (ledHue + encoderDelta * 8 + 256) % 256;
    uint8_t r, g, b; hsv2rgb(ledHue, r, g, b);
    apaShow(r, g, b, 20);
    gfx->fillRect(0, 28, SCR_W, SCR_H - 44, C_BG);
    gfx->fillRoundRect(90, 45, 140, 60, 10, C565(r, g, b));
    char t[24]; snprintf(t, sizeof(t), "R%d G%d B%d", r, g, b);
    textAt(95, 115, t, C_TEXT, 1);
  }
}

// =====================================================================
//  ЭКРАН: ФОНАРИК (белый экран + белый RGB)
// =====================================================================
void screenLight() {
  if (screenEntered) {
    gfx->fillScreen(C565(255, 255, 255));
    apaShow(255, 255, 255, 31);
    backlightSet(255);
    screenEntered = false;
  }
}

// =====================================================================
//  ЭКРАН: НАСТРОЙКИ (яркость экрана)
// =====================================================================
int setSel = 0;
void screenSettings() {
  if (screenEntered) {
    gfx->fillScreen(C_BG);
    drawHeader("Настройки");
    drawFooter("Крути: яркость   Долгое: назад");
    screenEntered = false; encoderDelta = 999;
  }
  if (encoderDelta) {
    if (encoderDelta != 999) {
      int v = constrain((int)g_backlight + encoderDelta * 15, 15, 255);
      backlightSet(v);
    }
    gfx->fillRect(0, 40, SCR_W, 60, C_BG);
    textAt(20, 46, "Яркость экрана", C_TEXT, 1);
    int bw = map(g_backlight, 0, 255, 0, SCR_W - 40);
    gfx->drawRoundRect(20, 66, SCR_W - 40, 16, 4, C_MUTED);
    gfx->fillRoundRect(20, 66, bw, 16, 4, C_ACCENT);
    char t[8]; snprintf(t, sizeof(t), "%d%%", map(g_backlight, 0, 255, 0, 100));
    textAt(SCR_W / 2 - 12, 90, t, C_TEXT, 1);
  }
}

// =====================================================================
//  БУТ-ЗАСТАВКА
// =====================================================================
void splash() {
  gfx->fillScreen(C_BG);
  for (int r = 8; r <= 44; r += 6)
    gfx->drawCircle(SCR_W / 2, 70, r, C565(20 + r, 120 + r, 60 + r / 2));
  gfx->fillCircle(SCR_W / 2, 70, 6, C_ACCENT);
  textAt(SCR_W / 2 - 78, 130, "T - E M B E D   L A B", C_TEXT, 2);
  for (int i = 0; i <= 255; i += 15) { backlightSet(i); delay(20); }
  delay(600);
}

// =====================================================================
//  SETUP / LOOP
// =====================================================================
void setup() {
  Serial.begin(115200);

  // 1) обязательно поднять питание платы
  pinMode(PIN_POWER_ON, OUTPUT);
  digitalWrite(PIN_POWER_ON, HIGH);
  delay(50);

  // 2) периферия
  backlightBegin();
  apaBegin();
  apaShow(0, 0, 0, 0);
  inputBegin();
  Wire.begin(PIN_IIC_SDA, PIN_IIC_SCL);
#if ENABLE_BME280
  bmeBegin();
#endif

  // 3) дисплей
  gfx->begin();
  SCR_W = gfx->width();
  SCR_H = gfx->height();
  initTheme();
  splash();

  backlightSet(g_backlight);
  screenEntered = true;
}

void loop() {
  inputPoll();

  // Долгое нажатие = назад в меню (кроме самого меню и фонарика)
  if (btnEvent == BTN_LONG && screen != SCR_MENU) {
    if (screen == SCR_LIGHT) backlightSet(g_backlight); // вернуть яркость
    if (screen == SCR_STOPWATCH) { swRun = false; swElapsed = 0; } // сброс
    screen = SCR_MENU;
    screenEntered = true;
    btnEvent = BTN_NONE;
  }

  switch (screen) {
    case SCR_MENU:      screenMenu();      break;
    case SCR_SYSINFO:   screenSysInfo();   break;
    case SCR_ENV:       screenEnv();       break;
    case SCR_CLOCK:     screenClock();     break;
    case SCR_STOPWATCH: screenStopwatch(); break;
    case SCR_WIFI:      screenWifi();      break;
    case SCR_LED:       screenLed();       break;
    case SCR_LIGHT:     screenLight();     break;
    case SCR_SETTINGS:  screenSettings();  break;
  }
  delay(5);
}
