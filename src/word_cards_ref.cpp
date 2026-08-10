/**
 * @file      word_cards.cpp
 * @brief     雅思单词卡（纯离线低功耗）：内置 ECDICT 雅思词库（PROGMEM），启动扫描建偏移索引，
 *            轻睡 60s 自动随机刷词（可按键手动刷新），快刷为主每 10 次全刷清残影，
 *            射频始终关闭（无 BLE/WiFi），轮数/计数 NVS 断电保持。
 *            换词库需重新生成 dict_*.h 并烧录。
 *
 * 用法: pio run -e word-cards -t upload
 */

#include <Arduino.h>
#include <string.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include <Preferences.h>
#include "dict_ielts.h"

// ---------------- 引脚 ----------------
#define PIN_EPD_CS    5
#define PIN_EPD_DC    17
#define PIN_EPD_RST   16
#define PIN_EPD_BUSY  4
#define PIN_BTN       39     // 按键，按下=低（轻睡唤醒手动刷新）
#define PIN_LED       19     // 低电平点亮（深睡期间保持关闭）
#define PIN_BAT_ADC   35     // 电池电压检测（分压比 ×2）

// ---------------- 屏幕 ----------------
GxEPD2_BW<GxEPD2_213_B73, GxEPD2_213_B73::HEIGHT> display(
    GxEPD2_213_B73(/*CS=*/PIN_EPD_CS, /*DC=*/PIN_EPD_DC,
                   /*RST=*/PIN_EPD_RST, /*BUSY=*/PIN_EPD_BUSY));
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

// ---------------- 词库索引 ----------------
// idx[n] = 第 n 个词条在 dictText 中的起始字节偏移（变长行，按索引 O(1) 定位）
static uint32_t idx[DICT_WORDS];
static uint16_t wordCount = 0;
static uint16_t currentIndex = 0;

// 状态变量（RAM，轻睡保留）：上次词索引、距上次全刷的快刷次数、本轮已刷词数
static uint16_t lastIndex = 0xFFFF;
static uint8_t  fastCount = 0;
static uint16_t roundCount = 0;     // 本轮已刷词数（右下角计数，NVS 恢复，满 5040 归 1）
static uint16_t cycleCount = 1;     // 总循环（第几轮）次数，NVS 恢复
static uint8_t  saveCount = 0;      // 距上次 NVS 保存的次数（每 10 次存一次省 flash）

typedef struct {
    char word[32];
    char phonetic[32];
    char meaning[192];      // 中文释义（含 \n，解析时已反转义）
} WordEntry;
static WordEntry cur;

/** 扫描 dictText，记录每个词条的起始偏移 */
static void buildIndex()
{
    wordCount = 0;
    uint32_t off = 0;
    while (dictText[off] && wordCount < DICT_WORDS) {
        idx[wordCount++] = off;
        while (dictText[off] && dictText[off] != '\n') off++;
        if (dictText[off]) off++;      // 跳过行尾 \n
    }
    Serial.printf("[wc] dict %u words\n", wordCount);
}

/** 读电池电压（GPIO35，分压比 ×2，16 次采样平均，上限 4.2V） */
static uint16_t readBatteryMv()
{
    uint32_t sum = 0;
    for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BAT_ADC);
    uint16_t mv = (uint16_t)(sum / 16) * 2;
    if (mv > 4200) mv = 4200;
    return mv;
}

// 聚合物锂电池放电曲线（电压 mV -> 百分比，按电压降序，两点间线性插值）
// 0% 从 LDO 最低工作电压 3.5V 起算（3.3V 输出 + 压差，低于此设备已断电）
// 特性：中段电压较平（3.85~3.7V 占 50%~26%），低段跌得快
static const uint16_t battCurve[][2] = {
    {4200, 100}, {4100, 88}, {4000, 75}, {3950, 66}, {3900, 58},
    {3850, 50}, {3800, 42}, {3750, 34}, {3700, 26}, {3650, 19},
    {3600, 13}, {3550, 7}, {3500, 0}
};
#define BATT_CURVE_N  (sizeof(battCurve) / sizeof(battCurve[0]))

/** 电池电压 -> 百分比（查表 + 线性插值） */
static uint8_t mvToPercent(uint16_t mv)
{
    if (mv >= battCurve[0][0]) return battCurve[0][1];                 // >=4.2V -> 100%
    if (mv <= battCurve[BATT_CURVE_N - 1][0]) return 0;                 // <=3.5V -> 0%
    for (int i = 0; i < BATT_CURVE_N - 1; i++) {
        if (mv >= battCurve[i + 1][0]) {
            uint32_t dV = battCurve[i][0] - battCurve[i + 1][0];
            uint32_t dP = battCurve[i][1] - battCurve[i + 1][1];
            uint32_t frac = (uint32_t)(mv - battCurve[i + 1][0]) * dP / dV;
            return battCurve[i + 1][1] + (uint8_t)frac;
        }
    }
    return 0;
}

/** 解析第 n 个词条到 cur（按 idx[n] 定位读单行） */
static void parseEntry(uint16_t n)
{
    const char* p = &dictText[idx[n]];
    int i = 0;

    // word
    while (*p && *p != '|' && *p != '\n' && i < 31) cur.word[i++] = *p++;
    cur.word[i] = 0;
    if (*p == '|') p++;

    // phonetic
    i = 0;
    while (*p && *p != '|' && *p != '\n' && i < 31) cur.phonetic[i++] = *p++;
    cur.phonetic[i] = 0;
    if (*p == '|') p++;

    // meaning（反转义 \\n -> \n）
    i = 0;
    while (*p && *p != '\n' && i < 191) {
        if (p[0] == '\\' && p[1] == 'n') { cur.meaning[i++] = '\n'; p += 2; }
        else cur.meaning[i++] = *p++;
    }
    cur.meaning[i] = 0;
}

// ---------------- UTF-8 工具 ----------------
static int utf8CharLen(const char* s)
{
    uint8_t c = (uint8_t)*s;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/** 中文释义：先按内嵌 \n 分段，再逐字符折行；y 超过 maxY 停止（防与页码重叠）
 *  用 getUTF8Width(整行) 测量，与实际渲染宽度一致，避免逐字符测量的提前量偏差导致超界 */
static void drawWrappedText(const char* text, int16_t startX, int16_t startY,
                            int16_t maxW, int16_t lineH, int16_t maxY)
{
    int16_t y = startY;
    const char* seg = text;
    while (seg && *seg) {
        if (y > maxY) return;               // 超出底部停止
        const char* nl = strchr(seg, '\n');
        int segLen = nl ? (int)(nl - seg) : (int)strlen(seg);

        char line[128];
        int  n = 0;
        for (int i = 0; i < segLen; ) {
            if (y > maxY) return;
            int clen = utf8CharLen(seg + i);
            memcpy(line + n, seg + i, clen);   // 试加当前字符
            n += clen;
            line[n] = 0;
            int16_t lineW = u8g2Fonts.getUTF8Width(line);
            if ((lineW > maxW && n > clen) || n >= 120) {   // 超宽 -> 撤掉当前字符，换行
                n -= clen;
                line[n] = 0;
                u8g2Fonts.setCursor(startX, y);
                u8g2Fonts.print(line);
                y += lineH;
                memcpy(line, seg + i, clen);   // 当前字符开新行
                n = clen;
                line[n] = 0;
            }
            i += clen;
        }
        if (n) {
            u8g2Fonts.setCursor(startX, y);
            u8g2Fonts.print(line);
            y += lineH;
        }
        if (!nl) break;
        seg = nl + 1;
    }
}

// ---------------- 渲染 ----------------
/** 渲染第 n 张卡 — 横屏 250×122，上下布局：
 *  单词/音标/分隔线在上，中文释义在下，页码右下角
 *  fast=true 局部快刷（~0.2s，轻微残影）；false 全刷（~4s，干净） */
static void renderCard(uint16_t n, bool fast)
{
    parseEntry(n);
    const WordEntry& e = cur;
    const int16_t W = display.width();    // 250（横屏）
    const int16_t H = display.height();   // 122
    const int16_t margin = 6;
    const int16_t contentW = W - 2 * margin;
    const uint8_t battPct = mvToPercent(readBatteryMv());   // 刷新前读电池

    if (fast) display.setPartialWindow(0, 0, W, H);   // 快刷
    else      display.setFullWindow();                 // 全刷

    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        u8g2Fonts.setFontMode(1);
        u8g2Fonts.setFontDirection(0);
        u8g2Fonts.setForegroundColor(GxEPD_BLACK);
        u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

        // 右上角电量：五段电池图标（无百分比文字）
        int16_t bx = W - margin - 18;                  // 电池图标右对齐
        int16_t by = 4;
        display.drawRect(bx, by, 18, 11, GxEPD_BLACK); // 电池壳
        display.fillRect(bx + 18, by + 3, 3, 5, GxEPD_BLACK); // 正极凸头
        int segs = (battPct + 19) / 20;                // 五段电池：每段 20%
        if (segs > 5) segs = 5;
        for (int s = 0; s < 5; s++) {
            int sx = bx + 2 + s * 3;                   // 2px 段 + 1px 间隙
            if (s < segs) display.fillRect(sx, by + 2, 2, 7, GxEPD_BLACK);
        }

        // 单词：通栏左对齐（238px 基本都能放下大字），超宽则降一号
        const uint8_t* wordFont = u8g2_font_helvB18_tf;
        u8g2Fonts.setFont(wordFont);
        u8g2Fonts.setFontMode(1);
        if (u8g2Fonts.getUTF8Width(e.word) > contentW) {
            wordFont = u8g2_font_helvB14_tf;
            u8g2Fonts.setFont(wordFont);
            u8g2Fonts.setFontMode(1);
        }
        u8g2Fonts.setCursor(margin, 26);
        u8g2Fonts.print(e.word);

        // 音标（左对齐）
        if (e.phonetic[0]) {
            u8g2Fonts.setFont(u8g2_font_helvR14_tf);
            u8g2Fonts.setFontMode(1);
            u8g2Fonts.setCursor(margin, 46);
            u8g2Fonts.print(e.phonetic);
        }

        // 分隔线
        display.drawFastHLine(margin, 54, contentW, GxEPD_BLACK);

        // 中文释义（短释义大字；行距+4；最多画到 y≈98，避免与页码重叠）
        const uint8_t* meaningFont =
            strlen(e.meaning) <= 60 ? u8g2_font_wqy16_t_gb2312 : u8g2_font_wqy14_t_gb2312;
        u8g2Fonts.setFont(meaningFont);
        u8g2Fonts.setFontMode(1);
        int16_t lineH = u8g2Fonts.getFontAscent() - u8g2Fonts.getFontDescent() + 4;  // 行距+4
        drawWrappedText(e.meaning, margin, 70, contentW, lineH, 98);

        // 页码：右下角
        char buf[20];
        snprintf(buf, sizeof(buf), "R%u %u/%u", cycleCount, roundCount, wordCount);
        u8g2Fonts.setFont(u8g2_font_7x13_tf);
        u8g2Fonts.setFontMode(1);
        int16_t cw = u8g2Fonts.getUTF8Width(buf);
        u8g2Fonts.setCursor(W - margin - cw, H - 8);
        u8g2Fonts.print(buf);
    } while (display.nextPage());
    display.hibernate();
    Serial.printf("[wc] card %u/%u %s\n", n + 1, wordCount, e.word);
}

/** 空词库占位（理论上不会出现） */
static void renderPlaceholder()
{
    const int16_t W = display.width();
    display.setFullWindow();
    display.firstPage();
    do {
        display.fillScreen(GxEPD_WHITE);
        u8g2Fonts.setFontMode(1);
        u8g2Fonts.setForegroundColor(GxEPD_BLACK);
        u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
        u8g2Fonts.setFont(u8g2_font_wqy14_t_gb2312);
        u8g2Fonts.setFontMode(1);
        const char* t1 = "词库为空";
        u8g2Fonts.setCursor((W - u8g2Fonts.getUTF8Width(t1)) / 2, 66);
        u8g2Fonts.print(t1);
    } while (display.nextPage());
    display.hibernate();
}

// ---------------- 低功耗配置（轻睡） ----------------
#define SLEEP_US    (60UL * 1000000UL)   // 轻睡 60s（参数单位是微秒）
#define FULL_EVERY  20                   // 每 20 次快刷全刷一次，清残影（省电）

/** 从 NVS 恢复本轮计数与上次词索引（断电保持；读写模式以创建命名空间） */
static void loadRoundCount()
{
    Preferences p;
    p.begin("wc", false);                 // 读写模式，首次自动创建命名空间
    roundCount = p.getUShort("round", 0);
    lastIndex   = p.getUShort("last", 0xFFFF);
    cycleCount  = p.getUShort("cycle", 1);
    p.end();
}

/** 把本轮计数与上次词索引存入 NVS（每 10 次刷新调一次，控制 flash 磨损） */
static void saveRoundCount()
{
    Preferences p;
    p.begin("wc", false);
    p.putUShort("round", roundCount);
    p.putUShort("last", lastIndex);
    p.putUShort("cycle", cycleCount);
    p.end();
}

/** 随机取下一个词并刷新（每 FULL_EVERY 次快刷全刷一次；每 10 次 NVS 保存一次） */
static void showNextWord()
{
    if (wordCount == 0) return;          // 空词库保护
    currentIndex = random(0, wordCount);
    if (wordCount > 1 && currentIndex == lastIndex)
        currentIndex = (currentIndex + 1) % wordCount;
    lastIndex = currentIndex;

    roundCount++;                        // 本轮计数 +1，满一轮归 1 并进入下一轮
    if (roundCount > wordCount) { roundCount = 1; cycleCount++; }

    if (++saveCount >= 10) { saveCount = 0; saveRoundCount(); }   // 断电保持，省 flash

    bool full;
    if (fastCount >= FULL_EVERY) { full = true; fastCount = 0; }
    else { full = false; fastCount++; }
    renderCard(currentIndex, !full);   // renderCard 第二参是 fast（true=快刷）
}

// ---------------- setup ----------------
void setup()
{
    Serial.begin(115200);
    delay(50);                        // 短延时（低功耗）
    randomSeed(esp_random());         // 随机种子（每次上电不同）
    pinMode(PIN_BTN, INPUT);          // 按键（GPIO39），轻睡时可唤醒手动刷新

    // 屏幕 + U8g2（横屏：旋转 270°，视口 250×122）
    display.init(115200, true, 2, false);
    display.setRotation(3);
    u8g2Fonts.begin(display);

    // 建索引 + 恢复断电前的计数 + 随机显示第一个词
    buildIndex();
    if (wordCount > 0) {
        loadRoundCount();                 // 从 NVS 恢复 roundCount/lastIndex
        showNextWord();                   // 首次：roundCount+1 并显示（GxEPD2 强制全刷）
        fastCount = 0;                    // 首次全刷不计入快刷计数
    } else {
        renderPlaceholder();
    }

    // 轻睡低功耗：断掉 RTC 快/慢存（状态在 DRAM 不受影响）；
    // RTC_PERIPH 保持供电（按键 ext0 唤醒需要 GPIO39）
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_FAST_MEM, ESP_PD_OPTION_OFF);
    esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_SLOW_MEM, ESP_PD_OPTION_OFF);

    Serial.println("[wc] light sleep 60s/word");
}

// ---------------- 主循环（轻睡 60s 或按键唤醒 -> 刷新下一个词） ----------------
void loop()
{
    // 轻睡：60s 定时器 + GPIO39 按键（低电平）都可唤醒
    esp_sleep_enable_timer_wakeup(SLEEP_US);
    esp_sleep_enable_ext0_wakeup(GPIO_NUM_39, 0);   // 按键按下唤醒
    esp_light_sleep_start();          // 保留状态，醒来继续

    showNextWord();                   // 刷新下一个随机词（快刷，每 10 次全刷）

    // 防连刷：无论哪种唤醒，按键若仍按下（按住/刚按下），等完全松开再睡。
    // 每次按下只刷一个词；按住不放则一直等，不会连刷。
    while (digitalRead(PIN_BTN) == LOW) delay(10);
}
