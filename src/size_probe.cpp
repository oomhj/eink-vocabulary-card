// size_probe.cpp — 临时尺寸探针：测 ESP8266 能否装下完整单词卡
// 链入：词库 dict_ielts.h（~247KB PROGMEM）+ 两个中文字体 wqy16/wqy14（~570KB）
//     + GxEPD2 显示对象 + u8g2 渲染，看链接器 Flash/RAM 报告。
// 不真运行（flash 直读字体会崩溃），只看链接尺寸。
#include <Arduino.h>
#include <string.h>
#include <GxEPD2_BW.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "dict_ielts.h"

#define PIN_EPD_CS   15
#define PIN_EPD_DC    5
#define PIN_EPD_RST   2
#define PIN_EPD_BUSY  4

GxEPD2_BW<GxEPD2_213, GxEPD2_213::HEIGHT> display(
    GxEPD2_213(PIN_EPD_CS, PIN_EPD_DC, PIN_EPD_RST, PIN_EPD_BUSY));
U8G2_FOR_ADAFRUIT_GFX u8g2Fonts;

static uint16_t wordCount = 0;

/** 第一趟：数词条数（两趟方案，不占 20KB 索引 RAM） */
static uint16_t countEntries()
{
    uint16_t n = 0;
    uint32_t off = 0;
    while (dictText[off] && n < DICT_WORDS) {
        n++;
        while (dictText[off] && dictText[off] != '\n') off++;
        if (dictText[off]) off++;
    }
    return n;
}

/** 解析第 n 条到 buf（word/phonetic/meaning，\\n 反转义） */
static void parseNth(uint16_t n, char* word, char* phon, char* meaning)
{
    uint32_t off = 0;
    for (uint16_t i = 0; i < n; i++) {
        while (dictText[off] && dictText[off] != '\n') off++;
        if (dictText[off]) off++;
    }
    int k = 0;
    while (dictText[off] && dictText[off] != '|' && dictText[off] != '\n' && k < 31) word[k++] = dictText[off++];
    word[k] = 0; if (dictText[off] == '|') off++;
    k = 0;
    while (dictText[off] && dictText[off] != '|' && dictText[off] != '\n' && k < 31) phon[k++] = dictText[off++];
    phon[k] = 0; if (dictText[off] == '|') off++;
    k = 0;
    while (dictText[off] && dictText[off] != '\n' && k < 191) {
        if (dictText[off] == '\\' && dictText[off+1] == 'n') { meaning[k++] = '\n'; off += 2; }
        else meaning[k++] = dictText[off++];
    }
    meaning[k] = 0;
}

void setup()
{
    Serial.begin(115200);
    wordCount = countEntries();
    char w[32], p[32], m[192];
    parseNth(0, w, p, m);
    Serial.printf("words=%u/%u first='%s'\n", wordCount, DICT_WORDS, w);
    // 只引用一个中文字体 wqy16（测"单字体方案"是否放得下）
    Serial.printf("wqy16=%p\n", (const void*)u8g2_font_wqy16_t_gb2312);
    Serial.println("SIZE PROBE OK");
}

void loop() {}
