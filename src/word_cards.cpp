/**
 * @file      word_cards.cpp
 * @brief     ESP8266 雅思单词卡固件（纯离线低功耗）
 *            行为：正常运行不睡眠——每 60s 自动刷一个随机词；SW2(IO0) 或 RST(SW4) 手动刷（一次=一个词）：
 *              - 上电/按 RST = 整机重启 = 刷一个随机词；之后常驻循环：60s 定时自动刷词，
 *                或按键 SW2(IO0) 刷词（睡眠策略见文件尾注记：深睡/FPM light sleep 均不可用）
 *              - 右上角电量图标（5 段），右下角 R<轮> <本轮词数>/<词库数>
 *              - 每次刷词正常全刷（无快刷/残影管理）；计数每次刷词存 LittleFS 断电保持
 *
 *            关键移植点（ESP8266）：
 *              - 驱动类可切：USE_SSD1680=1 → GxEPD2_213_B74(SSD1680)；=0 → GxEPD2_213_HINK
 *                (HINK-E0213A04-G01 实物 IL3895，克隆自库 GxEPD2_213，仅 VCOM 0x2C=0x18，见 hink_e0213.h)
 *              - 词库在 PROGMEM，所有读取用 pgm_read_byte（ESP8266 flash 只支持 32 位访问，
 *                单字节直读会 LoadStoreError；u8g2_fonts_flash.h 已把 u8x8_pgm_read 改为 pgm_read_byte）
 *              - 中文字体统一 wqy16（参考的双字号因 flash 1MB 上限只能留一个）
 *              - 上电/按 RST=整机重启；工作态每次唤醒存 LittleFS（无 RTC 内存操作）
 *
 * 用法: pio run -e esp8266 -t upload
 */
#include <Arduino.h>
#include <string.h>
#include <SPI.h>
#include <GxEPD2_BW.h>
#include <GxEPD2_3C.h>   // 三色屏（红/黑/白）GDEY0213Z98 用
#include <U8g2_for_Adafruit_GFX.h>
#include <LittleFS.h>
#include "dict_ielts.h"
#include "hink_e0213.h"   // HINK-E0213A04-G01 自定义 IL3895 类（VCOM 0x2C=0x18）

// ---------------- 屏幕选择 ----------------
// USE_Z98C=1  三色屏（红/黑/白）GDEY0213Z98（SSD1680，250×122）；无快刷，每次全刷 ~15s
// USE_SSD1680=1 SSD1680 屏（GxEPD2_213_B74，快刷好、对比度好）；
// 0 = HINK-E0213A04-G01（IL3895，自定义类 GxEPD2_213_HINK，VCOM 0x2C 实测调为 0x18）
#define USE_Z98C     0
#define USE_SSD1680  0

// ---------------- 调试 ----------------
// 生产可置 0：关掉 Serial 输出（省启动/刷新时间；GxEPD2 诊断也已关）
#define DEBUG_SERIAL  1

// ---------------- 引脚（两屏相同，实物确认） ----------------
#define PIN_EPD_CS   15
#define PIN_EPD_DC    5
#define PIN_EPD_RST   2
#define PIN_EPD_BUSY  4
#define PIN_BAT_EN    12    // 电池采样使能（HIGH 接通分压，见 hardware.md §10）

// ---------------- 屏幕 ----------------
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

// ---------------- 词库访问（PROGMEM，全用 pgm_read_byte） ----------------
static uint16_t wordCount = 0;

typedef struct {
    char word[32];
    char phonetic[32];
    char meaning[192];      // 中文释义（含 \n，解析时已反转义）
} WordEntry;
static WordEntry cur;

/** 第一趟：数词条数（两趟方案，不占 20KB 索引 RAM） */
static uint16_t countEntries()
{
    uint16_t n = 0;
    uint32_t off = 0;
    while (pgm_read_byte(&dictText[off]) && n < DICT_WORDS) {
        n++;
        while (pgm_read_byte(&dictText[off]) && pgm_read_byte(&dictText[off]) != '\n') off++;
        if (pgm_read_byte(&dictText[off])) off++;      // 跳过行尾 \n
    }
    return n;
}

/** 第二趟：解析第 n 条到 cur（word/phonetic/meaning，\\n 反转义） */
static void parseNth(uint16_t n)
{
    uint32_t off = 0;
    for (uint16_t i = 0; i < n; i++) {                  // 跳到第 n 条起点
        while (pgm_read_byte(&dictText[off]) && pgm_read_byte(&dictText[off]) != '\n') off++;
        if (pgm_read_byte(&dictText[off])) off++;
    }
    int k = 0;
    char c;
    while (k < 31 && (c = (char)pgm_read_byte(&dictText[off])) && c != '|' && c != '\n') { cur.word[k++] = c; off++; }
    cur.word[k] = 0;
    if (pgm_read_byte(&dictText[off]) == '|') off++;
    k = 0;
    while (k < 31 && (c = (char)pgm_read_byte(&dictText[off])) && c != '|' && c != '\n') { cur.phonetic[k++] = c; off++; }
    cur.phonetic[k] = 0;
    if (pgm_read_byte(&dictText[off]) == '|') off++;
    k = 0;
    while (k < 191 && (c = (char)pgm_read_byte(&dictText[off])) && c != '\n') {
        if (c == '\\' && pgm_read_byte(&dictText[off + 1]) == 'n') { cur.meaning[k++] = '\n'; off += 2; }
        else { cur.meaning[k++] = c; off++; }
    }
    cur.meaning[k] = 0;
}

// ---------------- UTF-8 工具（照抄参考固件） ----------------
static int utf8CharLen(const char* s)
{
    uint8_t c = (uint8_t)*s;
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

/** 中文释义：先按内嵌 \n 分段，再逐字符折行；y 超过 maxY 停止（防与页码重叠） */
static void drawWrappedText(const char* text, int16_t startX, int16_t startY,
                            int16_t maxW, int16_t lineH, int16_t maxY)
{
    int16_t y = startY;
    const char* seg = text;
    while (seg && *seg) {
        if (y > maxY) return;
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

// ---------------- 电池 ----------------
/** 聚合物锂电池放电曲线（电压 mV -> 百分比，两点间线性插值，照抄参考） */
static const uint16_t battCurve[][2] = {
    {4200, 100}, {4100, 88}, {4000, 75}, {3950, 66}, {3900, 58},
    {3850, 50}, {3800, 42}, {3750, 34}, {3700, 26}, {3650, 19},
    {3600, 13}, {3550, 7}, {3500, 0}
};
#define BATT_CURVE_N  (sizeof(battCurve) / sizeof(battCurve[0]))

/** 读电池电压（IO12 接通分压 → A0(TOUT 0–1V) → ×5.7 还原，上限 4.2V） */
static uint16_t readBatteryMv()
{
    digitalWrite(PIN_BAT_EN, HIGH);
    delay(5);                                         // 分压稳定（缩短节省活跃时间）
    uint32_t sum = 0;
    for (int i = 0; i < 8; i++) sum += analogRead(A0);
    digitalWrite(PIN_BAT_EN, LOW);
    uint16_t mv = (uint16_t)((uint32_t)(sum / 8) * 5700UL / 1023UL);    // ×1k/(4.7k+1k)=×5.7
    if (mv > 4200) mv = 4200;
    return mv;
}

/** 电池电压 -> 百分比（查表 + 线性插值，照抄参考） */
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

// ---------------- 状态（供渲染读页码） ----------------
typedef struct {
    uint32_t magic;        // RTC_STATE_MAGIC
    uint32_t randMix;      // 随机混合种子（LCG 每次唤醒自增 → RST 唤醒也各不相同）
    uint16_t roundCount;   // 本轮已刷词数（满 wordCount 归 1）
    uint16_t cycleCount;   // 总轮数
    uint16_t lastIndex;    // 上次词索引（防重复）
    uint16_t wordCount;    // 词库词条数（缓存，唤醒免扫词库第一趟）
    uint8_t  fastCount;    // 距上次全刷的快刷次数（满 FULL_EVERY 全刷清残影）
} State;   // 18B → 补齐 20B（4 对齐）
static State st;

// ---------------- 渲染 ----------------
/** 渲染当前词卡 — 横屏 250×122：
 *  单词/音标/分隔线在上，中文释义(wqy14)在下，页码右下角
 *  fast=true 整屏快刷（~0.5s，轻微残影）；false 全刷（~4s，干净，清残影）
 *  快刷后 EPD 保持上电（连续快刷无需重初始化）；全刷后 GxEPD2 自动 powerOff（RAM 保留，下次快刷自动 _Init_Part） */
static void renderCard(uint16_t n, bool fast)
{
    const int16_t W = display.width();    // 250（横屏）
    const int16_t H = display.height();   // 122
    const int16_t margin = 6;
    const int16_t contentW = W - 2 * margin;
    const uint8_t battPct = mvToPercent(readBatteryMv());   // 刷新前读电池

    display.firstPage();                  // 清缓冲为白（单遍绘制）
    {
        u8g2Fonts.setFontMode(1);
        u8g2Fonts.setFontDirection(0);
        u8g2Fonts.setForegroundColor(GxEPD_BLACK);
        u8g2Fonts.setBackgroundColor(GxEPD_WHITE);

        // 右上角电量：五段电池图标
        int16_t bx = W - margin - 18;
        int16_t by = 4;
        display.drawRect(bx, by, 18, 11, GxEPD_BLACK);           // 电池壳
        display.fillRect(bx + 18, by + 3, 3, 5, GxEPD_BLACK);    // 正极凸头
        int segs = (battPct + 19) / 20;
        if (segs > 5) segs = 5;
        for (int s = 0; s < 5; s++) {
            int sx = bx + 2 + s * 3;
            if (s < segs) display.fillRect(sx, by + 2, 2, 7, GxEPD_BLACK);
        }

        // 单词：通栏左对齐，超宽降一号
        const uint8_t* wordFont = u8g2_font_helvB18_tf;
        u8g2Fonts.setFont(wordFont);
        u8g2Fonts.setFontMode(1);
        if (u8g2Fonts.getUTF8Width(cur.word) > contentW) {
            wordFont = u8g2_font_helvB14_tf;
            u8g2Fonts.setFont(wordFont);
            u8g2Fonts.setFontMode(1);
        }
        u8g2Fonts.setCursor(margin, 26);
        u8g2Fonts.print(cur.word);

        // 音标（左对齐）；三色屏用红色，验证红平面
        if (cur.phonetic[0]) {
            u8g2Fonts.setFont(u8g2_font_helvR14_tf);
            u8g2Fonts.setFontMode(1);
#if USE_Z98C
            u8g2Fonts.setForegroundColor(GxEPD_RED);
#endif
            u8g2Fonts.setCursor(margin, 46);
            u8g2Fonts.print(cur.phonetic);
#if USE_Z98C
            u8g2Fonts.setForegroundColor(GxEPD_BLACK);   // 复位，后续内容仍黑色
#endif
        }

        // 分隔线
        display.drawFastHLine(margin, 54, contentW, GxEPD_BLACK);

        // 中文释义（统一 wqy14：wqy16 318KB 会超 1MB flash，wqy14 252KB；长释义靠换行）
        u8g2Fonts.setFont(u8g2_font_wqy14_t_gb2312);
        u8g2Fonts.setFontMode(1);
        int16_t lineH = u8g2Fonts.getFontAscent() - u8g2Fonts.getFontDescent() + 4;
        drawWrappedText(cur.meaning, margin, 70, contentW, lineH, 98);

        // 页码：右下角
        char buf[20];
        snprintf(buf, sizeof(buf), "R%u %u/%u", st.cycleCount, st.roundCount, wordCount);
        u8g2Fonts.setFont(u8g2_font_7x13_tf);
        u8g2Fonts.setFontMode(1);
        int16_t cw = u8g2Fonts.getUTF8Width(buf);
        u8g2Fonts.setCursor(W - margin - cw, H - 8);
        u8g2Fonts.print(buf);
    }

    // 刷新：fast=整屏快刷（display(true)，残影靠每 FULL_EVERY 次全刷清除）；
    //       full=全刷（display(false)，清残影）。不再 hibernate（连续快刷需保持控制器上电与 RAM）。
    display.display(fast);
    Serial.printf("[wc] card %u/%u %s (%s)\n", n + 1, wordCount, cur.word, fast ? "FAST" : "FULL");
}

/** 空词库占位 */
static void renderPlaceholder()
{
    const int16_t W = display.width();
    display.firstPage();
    {
        u8g2Fonts.setFontMode(1);
        u8g2Fonts.setFontDirection(0);
        u8g2Fonts.setForegroundColor(GxEPD_BLACK);
        u8g2Fonts.setBackgroundColor(GxEPD_WHITE);
        u8g2Fonts.setFont(u8g2_font_wqy14_t_gb2312);   // 复用 renderCard 已链入的 wqy14
        u8g2Fonts.setFontMode(1);
        const char* t1 = "词库为空";
        u8g2Fonts.setCursor((W - u8g2Fonts.getUTF8Width(t1)) / 2, 66);
        u8g2Fonts.print(t1);
    }
    display.display(false);
}

// ---------------- 状态持久化 ----------------
#define RTC_STATE_MAGIC  0x57433104     // "WC1\x04"
#define STATE_FILE       "/wc_state.bin"
#define FULL_EVERY       10              // 每 10 次快刷全刷一次，清残影（IL3895 残影积累快）
#define SLEEP_MS         (60UL * 1000UL)

static bool stateLoadFlash()
{
    if (!LittleFS.begin()) return false;
    File f = LittleFS.open(STATE_FILE, "r");
    bool ok = false;
    if (f) {
        if (f.readBytes((char*)&st, sizeof(st)) == sizeof(st) && st.magic == RTC_STATE_MAGIC) ok = true;
        f.close();
    }
    LittleFS.end();
    return ok;
}

static void stateSaveFlash()
{
    LittleFS.begin();
    File f = LittleFS.open(STATE_FILE, "w");
    if (f) { f.write((const uint8_t*)&st, sizeof(st)); f.close(); }
    LittleFS.end();
}

// ---------------- 取词与计数 ----------------
static void pickNextWord(bool forceFull)
{
    if (wordCount == 0) return;

    uint16_t n = random(0, wordCount);
    if (wordCount > 1 && n == st.lastIndex) n = (n + 1) % wordCount;
    st.lastIndex = n;

    st.roundCount++;                          // 本轮计数 +1，满一轮归 1 进下一轮
    if (st.roundCount > wordCount) { st.roundCount = 1; st.cycleCount++; }

    bool full = forceFull || (st.fastCount >= FULL_EVERY);   // 冷启动/计数到点 → 全刷清残影
    if (full) st.fastCount = 0; else st.fastCount++;

    parseNth(n);
    renderCard(n, !full);
}

// ---------------- setup（每次上电/按 RST 都整机重启跑到这里） ----------------
void setup()
{
#if DEBUG_SERIAL
    Serial.begin(74880);                 // 与 boot ROM 波特率一致，rst cause 与日志同屏可读
#endif
    delay(20);                                // 上电稳定（原 50ms 缩短）
#if DEBUG_SERIAL
    Serial.printf("[wc] reset: %s\n", ESP.getResetReason().c_str());
#endif

    // 状态加载：LittleFS → 默认（无 RTC 内存操作）
    if (!stateLoadFlash()) {
        memset(&st, 0, sizeof(st));
        st.magic = RTC_STATE_MAGIC;
        st.cycleCount = 1;
        st.lastIndex = 0xFFFF;
    }

    // 随机种子：ESP.random() 在 RF 关闭(RF_DISABLED)的 RST 唤醒下值不变 → 词会来回重复。
    // 混入 LittleFS 持久化的 randMix（LCG 每次唤醒自增），保证每次唤醒随机不同。
    st.randMix = st.randMix * 1664525u + 1013904223u;
    randomSeed(ESP.random() ^ st.randMix);

    // display.init 第一参传 0 = 关 GxEPD2 串口诊断（省刷新时的打印开销）；
    // 第二参恒 true：休眠唤醒也正常初始化屏幕（全刷清屏），无快刷/残影管理
    display.init(0, true, 2, false);
    display.setRotation(3);
    u8g2Fonts.begin(display);

    // 词库：唤醒时用 LittleFS 缓存的 wordCount，免第一趟扫描（省 ~50ms/次）；首次才扫
    wordCount = st.wordCount;
    if (wordCount == 0 || wordCount > DICT_WORDS) {
        wordCount = countEntries();
        st.wordCount = wordCount;
    }
#if DEBUG_SERIAL
    Serial.printf("[wc] dict %u words\n", wordCount);
#endif
    if (wordCount > 0) {
        pickNextWord(true);                     // 首次全刷（冷启动屏需干净），不计入快刷计数
    } else {
        renderPlaceholder();
    }

    stateSaveFlash();                         // 工作态存 LittleFS（断电保持，下次重启续用）

    // 睡眠策略（2026-08 定论）——不睡眠，正常运行：
    //   - 深睡（ESP.deepSleep）RTC 定时唤醒：boot 首跳挂死（ets 后无 load，"一次失败一次成功"），
    //     pinMode(16, WAKEUP_PULLUP) 实测未修复；
    //   - FPM light sleep（wifi_fpm_do_sleep）：RF 关=不睡；RF 开=fpm_do_sleep 空指针崩溃
    //     （Arduino core 3.x 移除 ESP.lightSleep() 的原因）。
    //   → 直接正常运行：60s 定时自动刷词 + SW2(IO0) 按键刷词 + RST(SW4) 整机重启刷词。
#if DEBUG_SERIAL
    Serial.println("[wc] run; 60s/word, SW2(IO0) or RST(SW4) for next");
#endif
    pinMode(0, INPUT_PULLUP);            // SW2 上键（IO0，串 R5 100Ω 接地，按下为低）
    uint32_t lastMs = millis();
    bool wantWord = false;
    for ( ;; ) {
        if (digitalRead(0) == LOW) {                 // SW2 按下
            while (digitalRead(0) == LOW) delay(50); // 等松开（一次按下=一个词）
            wantWord = true;
        }
        if (wantWord || millis() - lastMs >= SLEEP_MS) {  // 按键 或 60s 到：刷一个词
            wantWord = false;
            lastMs = millis();
            pickNextWord(false);                // 快/全刷由 st.fastCount 决定（每 FULL_EVERY 次全刷清残影）
            stateSaveFlash();
        }
        delay(50);
    }
}

void loop() {}
