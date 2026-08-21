/**
 * @file      screen_test.cpp
 * @brief     屏幕测试固件（screen-test 分支专用，不带单词卡逻辑）
 *
 *            纯屏测：全刷/快刷计时、黑白/棋盘格/线/文字/中文渲染、
 *            连续快刷残影观察。
 *            跑完一轮 → 常驻等待（不深睡），按 SW4/RST 手动重跑。
 *            每次 RST=整机重启，串口可见 boot ROM 的 rst cause/boot mode 与
 *            [t] reset 原因——用于排查 RST 启动可靠性（排除深睡变量）。
 *
 * 用法: pio run -e esp8266 -t upload
 * 观察: pio device monitor  @74880
 */
#include <Arduino.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>   // 三色屏 GDEY0213Z98 用
#include <U8g2_for_Adafruit_GFX.h>
#include "hink_e0213.h"   // HINK-E0213A04-G01 自定义 IL3895 类（VCOM 0x2C=0x18）

// ---------------- 屏幕选择（同 word_cards.cpp） ----------------
#define USE_Z98C     0    // 1=三色屏（红/黑/白）GDEY0213Z98
#define USE_SSD1680  0    // 1=SSD1680（GxEPD2_213_B74）
                          // 0=HINK-E0213A04-G01（IL3895，默认）
#define PIN_EPD_CS   15
#define PIN_EPD_DC    5
#define PIN_EPD_RST   2
#define PIN_EPD_BUSY  4

#if USE_Z98C
GxEPD2_3C<GxEPD2_213_Z98c, GxEPD2_213_Z98c::HEIGHT> display(
    GxEPD2_213_Z98c(/*CS=*/PIN_EPD_CS, /*DC=*/PIN_EPD_DC,
                    /*RST=*/PIN_EPD_RST, /*BUSY=*/PIN_EPD_BUSY));
#elif USE_SSD1680
GxEPD2_BW<GxEPD2_213_B74, GxEPD2_213_B74::HEIGHT> display(
    GxEPD2_213_B74(/*CS=*/PIN_EPD_CS, /*DC=*/PIN_EPD_DC,
                   /*RST=*/PIN_EPD_RST, /*BUSY=*/PIN_EPD_BUSY));
#else
GxEPD2_BW<GxEPD2_213_HINK, GxEPD2_213_HINK::HEIGHT> display(
    GxEPD2_213_HINK(/*CS=*/PIN_EPD_CS, /*DC=*/PIN_EPD_DC,
                    /*RST=*/PIN_EPD_RST, /*BUSY=*/PIN_EPD_BUSY));
#endif
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

#define W  display.width()     // 250（横屏）
#define H  display.height()    // 122

// ---------------- 单幅测试：firstPage 内执行 draw()，fast?快刷:全刷，打印耗时 ----------------
template <typename F>
static void stage(const char* name, bool fast, F draw)
{
    display.firstPage();
    { draw(); }
    uint32_t t0 = micros();
    display.display(fast);
    Serial.printf("[t] %-16s %lu ms\n", name, (unsigned long)((micros() - t0) / 1000));
    delay(200);                                  // 停留便于肉眼观察
}

// ---------------- 测试幅面 ----------------
static void drawWhite() { display.fillScreen(GxEPD_WHITE); }
static void drawBlack() { display.fillScreen(GxEPD_BLACK); }

static void drawGrid()
{
    display.fillScreen(GxEPD_WHITE);
    display.drawRect(2, 2, W - 4, H - 4, GxEPD_BLACK);      // 边框
    for (int x = 8; x < W; x += 16) display.drawFastVLine(x, 4, H - 8, GxEPD_BLACK);
    for (int y = 8; y < H; y += 16) display.drawFastHLine(4, y, W - 8, GxEPD_BLACK);
}

static void drawChecker()
{
    display.fillScreen(GxEPD_WHITE);
    for (int y = 0; y < H; y += 2)
        for (int x = (y / 2) & 1; x < W; x += 2)
            display.fillRect(x, y, 1, 1, GxEPD_BLACK);      // 1px 棋盘（伪灰阶，检对比度）
}

static void drawLines()
{
    display.fillScreen(GxEPD_WHITE);
    for (int i = 0; i < 10; i++)
        display.fillRect(4 + i * 12, 4, (i % 2) + 1, H - 8, GxEPD_BLACK);  // 粗细交替竖条
}

static void drawTextAscii()
{
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setFontDirection(0);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
    u8g2Fonts.setFont(u8g2_font_helvB18_tf);
    u8g2Fonts.setCursor(8, 30);
    u8g2Fonts.print("Screen Test");
    u8g2Fonts.setFont(u8g2_font_helvR14_tf);
    u8g2Fonts.setCursor(8, 55);
    u8g2Fonts.print("250x122 IL3895");
    u8g2Fonts.setFont(u8g2_font_7x13_tf);
    u8g2Fonts.setCursor(8, 80);
    u8g2Fonts.print("full ~3.9s / fast ~0.2s");
    u8g2Fonts.setCursor(8, 100);
    u8g2Fonts.print("ABCDEFG 1234567890");
}

static void drawTextCn()
{
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
    u8g2Fonts.setFont(u8g2_font_wqy14_t_gb2312);
    u8g2Fonts.setCursor(8, 30);
    u8g2Fonts.print("屏幕测试");
    u8g2Fonts.setCursor(8, 54);
    u8g2Fonts.print("雅思单词卡 2.13inch");
    u8g2Fonts.setCursor(8, 78);
    u8g2Fonts.print("中文字体 wqy14");
    u8g2Fonts.setCursor(8, 102);
    u8g2Fonts.print("快刷残影观察测试");
}

static int g_fastSeq = 0;
static void drawFast()
{
    g_fastSeq++;
    display.fillScreen(GxEPD_WHITE);
    u8g2Fonts.setFontMode(1);
    u8g2Fonts.setForegroundColor(GxEPD_BLACK);
    u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
    u8g2Fonts.setFont(u8g2_font_helvB18_tf);
    u8g2Fonts.setCursor(8, 40);
    u8g2Fonts.print("FAST");
    u8g2Fonts.setFont(u8g2_font_helvR14_tf);
    char buf[24];
    snprintf(buf, sizeof(buf), "#%02d  ghost test", g_fastSeq);
    u8g2Fonts.setCursor(8, 70);
    u8g2Fonts.print(buf);
    u8g2Fonts.setFont(u8g2_font_7x13_tf);
    u8g2Fonts.setCursor(8, 100);
    u8g2Fonts.print("fast refresh x10");
}

// ---------------- setup：一轮测试 → 常驻（不深睡），手动 RST 重跑 ----------------
void setup()
{
    Serial.begin(74880);                 // 与 boot ROM 波特率对齐
    delay(20);
    Serial.printf("[t] reset: %s\n", ESP.getResetReason().c_str());
    Serial.printf("[t] %dx%d screen test start\n", W, H);

    display.init(0, true, 2, false);     // 第一参 0=关 GxEPD2 诊断；全刷初始化
    display.setRotation(3);              // 横屏
    u8g2Fonts.begin(display);

    // 全刷系列
    stage("white full",  false, drawWhite);
    stage("black full",  false, drawBlack);
    stage("grid",        false, drawGrid);
    stage("checker 1px", false, drawChecker);
    stage("lines",       false, drawLines);
    stage("text ascii",  false, drawTextAscii);
    stage("text cn",     false, drawTextCn);

    // 快刷系列（连续 10 次，肉眼观察残影累积）
    g_fastSeq = 0;
    for (int i = 0; i < 10; i++)
        stage("fast", true, drawFast);
    stage("white clear", false, drawWhite);    // 全刷清残影

    Serial.println("[t] done — press SW4/RST to re-run");
    for (;;) delay(1000);            // 常驻等待，不深睡；RST 整机重启重跑
}

void loop() {}
