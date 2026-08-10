/**
 * @file      screen_demo.cpp
 * @brief     屏幕 + 文本渲染测试（ESP8266）
 *            GxEPD2 驱动类：GxEPD2_213（GDE0213B1 / IL3895），横屏 250×122
 *            引脚：SDA=IO13  CLK=IO14  CS=IO15  RST=IO2  BUSY=IO4  DC=IO5
 *
 *            全刷显示：外框 + 四角黑块 + 中央英文文本（u8g2 渲染）。
 *            文本用 ASCII 字体（helvB18，进 RAM），避开 flash 字体直读崩溃问题。
 *
 * 用法: pio run -e esp8266 -t upload
 */
#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>

// ---------------- 引脚 ----------------
#define PIN_EPD_CS   15
#define PIN_EPD_DC    5
#define PIN_EPD_RST   2
#define PIN_EPD_BUSY  4

// ---------------- 屏幕：横屏 250×122 ----------------
GxEPD2_BW<GxEPD2_213, GxEPD2_213::HEIGHT> display(
    GxEPD2_213(/*CS=*/PIN_EPD_CS, /*DC=*/PIN_EPD_DC,
               /*RST=*/PIN_EPD_RST, /*BUSY=*/PIN_EPD_BUSY));
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

/** 画一屏：外框 + 四角黑块 + 中央文本 */
static void drawTextScreen(const char* line1, const char* line2)
{
    const int16_t W = display.width();    // 250
    const int16_t H = display.height();   // 122

    display.firstPage();                  // 清缓冲为白
    {
        display.drawRect(1, 1, W - 2, H - 2, GxEPD_BLACK);        // 外框
        display.fillRect(8, 8, 20, 20, GxEPD_BLACK);              // 四角黑块
        display.fillRect(W - 28, 8, 20, 20, GxEPD_BLACK);
        display.fillRect(8, H - 28, 20, 20, GxEPD_BLACK);
        display.fillRect(W - 28, H - 28, 20, 20, GxEPD_BLACK);

        u8g2Fonts.setFontMode(1);
        u8g2Fonts.setFontDirection(0);
        u8g2Fonts.setForegroundColor(GxEPD_BLACK);
        u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

        // 第一行大字
        u8g2Fonts.setFont(u8g2_font_helvB18_tf);
        int16_t w1 = u8g2Fonts.getUTF8Width(line1);
        u8g2Fonts.setCursor((W - w1) / 2, 52);
        u8g2Fonts.print(line1);

        // 第二行小字
        if (line2[0]) {
            u8g2Fonts.setFont(u8g2_font_helvR14_tf);
            int16_t w2 = u8g2Fonts.getUTF8Width(line2);
            u8g2Fonts.setCursor((W - w2) / 2, 82);
            u8g2Fonts.print(line2);
        }
    }
}

static int idx = 0;

void setup()
{
    Serial.begin(115200);
    delay(50);
    Serial.printf("[screen] reset: %s\n", ESP.getResetReason().c_str());
    Serial.println("[screen] text test / GxEPD2_213 (IL3895)");

    display.init(115200, true, 2, false);   // 冷启动
    display.setRotation(3);
    u8g2Fonts.begin(display);

    // 清屏
    display.fillScreen(GxEPD_BLACK);
    display.display(false);
    display.fillScreen(GxEPD_WHITE);
    display.display(false);
    Serial.println("[screen] clear done");
    display.hibernate();
    delay(2000);
}

void loop()
{
    const char* texts[][2] = {
        { "IELTS",         "2.13 inch e-Paper" },
        { "HELLO",         "SSD1675B / IL3895" },
        { "250x122",       "full refresh" },
    };
    const int N = 3;
    display.init(115200, true, 2, false);   // 全刷
    display.setRotation(3);
    drawTextScreen(texts[idx % N][0], texts[idx % N][1]);
    display.display(false);                 // 全刷
    display.hibernate();
    Serial.printf("[screen] text #%d: %s\n", idx % N, texts[idx % N][0]);
    idx++;
    delay(5000);
}
