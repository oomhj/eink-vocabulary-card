/**
 * @file      word_cards.cpp
 * @brief     ESP8266 雅思单词卡固件（纯离线低功耗）
 *            行为对齐参考固件 src/word_cards_ref.cpp（ESP32 参考，行为基准）：
 *              - 深睡 60s 自动刷一个随机词；SW4(唤醒键/RST) 手动刷（一次按下=一个词）
 *              - 右上角电量图标（5 段），右下角 R<轮> <本轮词数>/<词库数>
 *              - 快刷为主，每 10 次全刷清残影（FULL_EVERY=10）；计数 RTC+LittleFS 断电保持
 *
 *            关键移植点（ESP8266）：
 *              - 驱动类可切：USE_SSD1680=1 → GxEPD2_213_B74(SSD1680)；=0 → GxEPD2_213_HINK
 *                (HINK-E0213A04-G01 实物 IL3895，克隆自库 GxEPD2_213，仅 VCOM 0x2C=0x18，见 hink_e0213.h)
 *              - 词库在 PROGMEM，所有读取用 pgm_read_byte（ESP8266 flash 只支持 32 位访问，
 *                单字节直读会 LoadStoreError；u8g2_fonts_flash.h 已把 u8x8_pgm_read 改为 pgm_read_byte）
 *              - 中文字体统一 wqy14（参考的双字号因 flash 1MB 上限只能留一个）
 *              - 深睡唤醒=整机重启，工作态存 RTC 内存，断电保持存 LittleFS
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
#define USE_PARTIAL_WINDOW 0     // 局刷：只刷内容区窗口；实测 IL3895 子窗口局刷花屏，暂禁用（0=整屏快刷）

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
/**
 * 分压链（2026-08 改版电路，见 电量检测.png / hardware.md §10）：
 *   BAT+ → Q5(SI2305 P-MOS，高边开关) → R15(100K) → (ADC 结点) → R16(30K) → GND
 *   IO12(=0V)：Q1(SI2302) 截止 → Q5 栅极=BAT+(高) → Q5 关断，采样回路断开，静态≈0
 *   IO12(=3.3V)：Q1 导通 → Q5 栅极拉至 GND → Q5 导通(V_GS≈-V_BAT) → BAT+ 接入分压
 *   R14 不在分压链（它是 Q5 栅极拉到 BAT+ 的默认关断电阻，只起门控），C20(10nf) 滤波。
 *   V_adc = V_bat × R16/(R15+R16) = ×30/130 = 0.231，故 V_bat = V_adc × 130/30 = ×4.333。
 *   ★ ESP8266 ADC(TOUT) 量程 0–1.0V（非 3.3V）：比 0.231 保证 4.2V 电池时节点 0.97V 不削顶（冲顶点 4.33V）。
 *     旧 R16=150K(比0.6) 时 4.2V 节点 2.5V 超 1.0V ADC → raw 削顶 1024，故 R16 改 30K。
 */
#define BATT_DIV_NUM  130UL    // R15+R16（kΩ）= 100+30（R14 不参与分压）
#define BATT_DIV_DEN  30UL     // R16（ADC 结点到 GND，kΩ）= 3×10K 串联
#define BATT_ADC_VREF 1000UL   // ADC 满量程 mV（ESP8266 TOUT 内部基准 ~1.0V，实测确认）
#define BATT_SAMPLE_N 16       // 采样次数（取均值降噪）

/** 聚合物锂电池放电曲线（电压 mV -> 百分比，两点间线性插值，照抄参考） */
static const uint16_t battCurve[][2] = {
    {4200, 100}, {4100, 88}, {4000, 75}, {3950, 66}, {3900, 58},
    {3850, 50}, {3800, 42}, {3750, 34}, {3700, 26}, {3650, 19},
    {3600, 13}, {3550, 7}, {3500, 0}
};
#define BATT_CURVE_N  (sizeof(battCurve) / sizeof(battCurve[0]))

/** 读电池电压（IO12 栅控导通 Q5 高边开关 → A0 采样 → 按分压比 ×130/30 还原，上限 4.2V）
 *  ESP8266 ADC 量程 0–1.0V：分压比 30/130 保证 4.2V 电池时节点 0.97V（不削顶）。 */
static uint16_t readBatteryMv()
{
    digitalWrite(PIN_BAT_EN, HIGH);                   // Q1 导通，接通 BAT+ 分压链
    delay(30);                                        // C20(10nf)×分压戴维南等效(R15‖R16≈23k) 稳定（τ≈0.23ms，留裕量）
    uint32_t sum = 0;
    for (int i = 0; i < BATT_SAMPLE_N; i++) sum += analogRead(A0);
    digitalWrite(PIN_BAT_EN, LOW);                    // 断开分压链，省活跃电流
    uint16_t raw = (uint16_t)(sum / BATT_SAMPLE_N);
    uint32_t mv = (uint32_t)raw * BATT_ADC_VREF * BATT_DIV_NUM / (1023UL * BATT_DIV_DEN);  // ×130/30
    if (mv > 4200) mv = 4200;
#if DEBUG_SERIAL
    Serial.printf("[wc] batt raw=%u → %umV\n", raw, (unsigned)mv);   // 定标：对照万用表电池电压
#endif
    return (uint16_t)mv;
}

/** 电池采样诊断（setup 里跑一次）：IO12 拉高/拉低分别读 raw
 *  raw(HIGH) 明显高于 raw(LOW) → 使能导通正常，看数值定分压比/V_ref
 *  raw(HIGH)≈raw(LOW)≈15 → Q5 没导通 / 电池没到 BAT+ / A0 没接分压结点 */
static void battDiag()
{
    uint32_t sumH = 0, sumL = 0;
    digitalWrite(PIN_BAT_EN, HIGH); delay(80);
    for (int i = 0; i < 16; i++) sumH += analogRead(A0);
    digitalWrite(PIN_BAT_EN, LOW);  delay(80);
    for (int i = 0; i < 16; i++) sumL += analogRead(A0);
    Serial.printf("[wc] batt DIAG: raw(IO12 HIGH)=%u  raw(IO12 LOW)=%u\n",
                  (unsigned)(sumH / 16), (unsigned)(sumL / 16));
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
    uint8_t  fastCount;    // 距上次全刷的快刷次数
    uint8_t  saveCount;    // 距上次 LittleFS 保存次数
} State;   // 编译器补齐到 20B（RTC 读写要求 4 对齐）
static State st;

// ---------------- 渲染 ----------------
/** 渲染当前词卡 — 横屏 250×122：
 *  单词/音标/分隔线在上，中文释义(wqy16)在下，页码右下角
 *  fast=true 快刷；false 全刷（由 display(false/true) 决定，含 _Init_Part 触发） */
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

    // 刷新：
    //   fast=true + 局刷  → setPartialWindow 只刷内容区窗口（排除右下角页码，缩小残影面）+ 快刷 LUT
    //   fast=true + 非局刷 → display(true) 整屏快刷
    //   fast=false         → display(false) 全刷。随后屏深睡。
    if (fast && USE_PARTIAL_WINDOW) {
        display.setPartialWindow(0, 0, W, 104);   // 局刷窗口：内容区（词/音标/释义到 y≈98）
        display.nextPage();                        // _pages==1：写缓冲 + 局刷刷新（含快刷第二相位）
        display.setFullWindow();                   // 复位窗口，避免下次全刷错乱
    } else {
        display.display(fast);
    }
    display.hibernate();
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
    display.hibernate();
}

// ---------------- 状态持久化 ----------------
#define RTC_STATE_MAGIC  0x57433102     // "WC1\x02"（+1 使旧状态失效，强制重扫词库+重置随机）
#define SLEEP_US         (60UL * 1000000UL)
#define FULL_EVERY       10              // 每 10 次快刷全刷一次，清残影（IL3895 残影积累快，20 次太脏）
#define SAVE_EVERY       10              // 每 10 次刷新存一次 LittleFS，省 flash 磨损
#define STATE_FILE       "/wc_state.bin"

static bool stateLoadRtc()
{
    if (ESP.rtcUserMemoryRead(0, (uint32_t*)&st, sizeof(st)))
        if (st.magic == RTC_STATE_MAGIC) return true;
    return false;
}

static void stateSaveRtc()
{
    ESP.rtcUserMemoryWrite(0, (uint32_t*)&st, sizeof(st));
}

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

    if (++st.saveCount >= SAVE_EVERY) { st.saveCount = 0; stateSaveFlash(); }   // 断电保持

    bool full = forceFull || (st.fastCount >= FULL_EVERY);   // 冷启动或快刷计数到点 → 全刷
    if (full) st.fastCount = 0; else st.fastCount++;

    parseNth(n);
    renderCard(n, !full);                       // renderCard 第二参是 fast（true=快刷）
}

// ---------------- setup（每次深睡唤醒都整机重启跑到这里） ----------------
void setup()
{
#if DEBUG_SERIAL
    Serial.begin(74880);                 // 与 boot ROM 波特率一致，rst cause 与日志同屏可读
#endif
    delay(20);                                // 上电稳定（原 50ms 缩短）
    pinMode(PIN_BAT_EN, OUTPUT);              // IO12 电池采样使能：必须 OUTPUT，否则 digitalWrite 不真驱动（ESP8266 复位后 GPIO 默认高阻输入），Q1/Q5 导通不了 → 采样≈0
    digitalWrite(PIN_BAT_EN, LOW);            // 默认低 = 不采样（Q5 关断，静态≈0）
#if DEBUG_SERIAL
    Serial.printf("[wc] reset: %s\n", ESP.getResetReason().c_str());
#endif
    battDiag();                                        // 电池采样诊断（raw IO12 高/低）

    // 状态加载：RTC → LittleFS → 默认
    bool rtcOk = stateLoadRtc();
    bool coldBoot = !rtcOk;
    if (!rtcOk) {
        if (!stateLoadFlash()) {
            memset(&st, 0, sizeof(st));
            st.magic = RTC_STATE_MAGIC;
            st.cycleCount = 1;
            st.lastIndex = 0xFFFF;
        }
    }

    // 随机种子：ESP.random() 在 RF 关闭(RF_DISABLED)的 RST 唤醒下值不变 → 词会来回重复。
    // 混入 RTC 持久化的 randMix（LCG 每次唤醒自增），保证每次唤醒随机不同。
    st.randMix = st.randMix * 1664525u + 1013904223u;
    randomSeed(ESP.random() ^ st.randMix);

    // 全刷/快刷由 pickNextWord 决定（renderCard 里 display.display(fast) 单次刷新）。
    // display.init 第二参 initial 恒传 **false**：
    //   若传 true（coldBoot||wantFull）→ _initial_write=true → writeImage 里先 writeScreenBuffer→clearScreen
    //   （clearScreen 在 _initial_refresh=true 时做 _Init_Full+_Update_Full 白屏全刷 + 末尾 _Init_Part+_Update_Part 白屏快刷），
    //   再叠加 refresh(false) 的内容 _Update_Full = 全刷实际刷 2~3 遍（~8s）。initial=false 只刷 1 遍（快刷一直正常就是走这条）。
    display.init(0, false, 2, false);   // 第一参 0 = 关 GxEPD2 串口诊断
    display.setRotation(3);
    u8g2Fonts.begin(display);

    // 词库：唤醒时用 RTC 缓存的 wordCount，免第一趟扫描（省 ~50ms/次）；冷启动才扫
    wordCount = st.wordCount;
    if (wordCount == 0 || wordCount > DICT_WORDS) {
        wordCount = countEntries();
        st.wordCount = wordCount;
    }
#if DEBUG_SERIAL
    Serial.printf("[wc] dict %u words\n", wordCount);
#endif
    if (wordCount > 0) {
        pickNextWord(coldBoot);                 // 冷启动强制全刷（清屏需干净）；full/fast 由 pickNextWord 决定
    } else {
        renderPlaceholder();
    }

    stateSaveRtc();                           // 工作态存 RTC（跨深睡）
#if DEBUG_SERIAL
    Serial.println("[wc] deep sleep 60s/word");
#endif
    ESP.deepSleep(SLEEP_US, WAKE_RF_DISABLED);   // 不返回；SW4(RST) 可立即唤醒
}

void loop() {}
