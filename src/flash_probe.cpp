// flash_probe.cpp — 诊断 flash 直读崩溃
// 目的：分清是"高地址 flash 数据直读根本不行"（词库也要走文件系统）
//       还是"字体/特定段有问题"（可修，词库留代码）。
// 读词库 dictText（标准 PROGMEM，.irom.text）在低/中/高偏移 + 读字体，看哪个崩。
#include <Arduino.h>
#include <U8g2_for_Adafruit_GFX.h>
#include "dict_ielts.h"

extern const uint8_t u8g2_font_helvB18_tf[];

static void rd(const char* tag, uint32_t off)
{
    uint32_t val = 0;
    for (int i = 0; i < 8; i++) val = val * 31 + pgm_read_byte(&dictText[off + i]);   // 32 位读机制
    Serial.printf("dictText[%u] addr=%p -> %08x  [%s] OK\n", off,
                  (const void*)&dictText[off], (unsigned)val, tag);
}

void setup()
{
    Serial.begin(115200);
    delay(200);
    Serial.println("=== flash read probe ===");

    rd("low",    0);
    rd("mid",    50000);
    rd("high",   150000);
    rd("higher", 230000);

    Serial.printf("font addr: %p\n", (const void*)u8g2_font_helvB18_tf);
    uint32_t acc = 0;
    for (int i = 0; i < 32; i++) acc = acc * 31 + pgm_read_byte(&u8g2_font_helvB18_tf[i]);   // 32 位读机制
    Serial.printf("font[0..31] sum=%08x  [font] OK\n", (unsigned)acc);

    Serial.println("PROBE DONE - all reads OK");
}

void loop() {}
