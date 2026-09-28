#pragma once
// =====================================================================
//  Распиновка LILYGO T-Embed (ESP32-S3)
//  Источник: официальный репозиторий Xinyuan-LilyGO/T-Embed
//  example/tft/pin_config.h
//
//  ВАЖНО: если у тебя версия T-Embed CC1101, часть пинов может
//  отличаться. Если экран не заработает — напиши мне, подставим
//  распиновку для CC1101.
// =====================================================================

/* Питание платы: этот пин ОБЯЗАТЕЛЬНО поднять в HIGH,
   иначе не включится ни экран, ни периферия. */
#define PIN_POWER_ON     46

/* Встроенная шина I2C (для датчиков BME280 и т.п.) */
#define PIN_IIC_SDA      18
#define PIN_IIC_SCL      8

/* Адресные светодиоды APA102 (RGB-подсветка) */
#define PIN_APA102_CLK   45
#define PIN_APA102_DI    42

/* Поворотный энкодер + кнопка (нажатие на энкодер) */
#define PIN_ENCODE_A     2
#define PIN_ENCODE_B     1
#define PIN_ENCODE_BTN   0

/* Дисплей ST7789 (SPI), 170x320 */
#define PIN_LCD_BL       15   // подсветка
#define PIN_LCD_DC       13
#define PIN_LCD_CS       10
#define PIN_LCD_CLK      12
#define PIN_LCD_MOSI     11
#define PIN_LCD_RES      9

/* Замер напряжения аккумулятора (АЦП) */
#define PIN_BAT_VOLT     4

/* Микрофон I2S / кодек ES7210 (в этой прошивке не используется,
   оставлено для справки и будущих модулей) */
#define PIN_IIS_BCLK     7
#define PIN_IIS_WCLK     5
#define PIN_IIS_DOUT     6
#define PIN_ES7210_BCLK  47
#define PIN_ES7210_LRCK  21
#define PIN_ES7210_DIN   14
#define PIN_ES7210_MCLK  48

/* microSD (SPI) */
#define PIN_SD_CS        39
#define PIN_SD_SCK       40
#define PIN_SD_MOSI      41
#define PIN_SD_MISO      38
