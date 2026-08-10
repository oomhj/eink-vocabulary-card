// src/u8g2_fonts_flash.h — 强制 -include 到所有编译单元（platformio.ini build_flags）
//
// 解决两个 ESP8266 问题：
//
// 1) U8g2_for_Adafruit_GFX 只对 AVR 把字体放进 flash；ESP8266 下 const 字体默认进
//    .rodata(RAM)，而 wqy16/wqy14 中文字体 ~570KB 会溢出 80KB RAM。
//    → 强制 U8X8_FONT_SECTION 把字体重定位到 flash 的 .irom.text，且每字一个独立
//      section（否则所有字体挤同一段，--gc-sections 无法逐字丢弃未用字体）。
//
// 2) ESP8266 的 flash 映射区（ICACHE 0x40200000+）只支持 32 位对齐访问，8/16 位
//    直接读取会触发 LoadStoreError（Exception 3）。u8g2 的 u8x8_pgm_read 默认是
//    `*(const uint8_t*)(adr)` 单字节直读 → 字体在 flash 时崩溃。
//    → 重定义 u8x8_pgm_read 为 pgm_read_byte（内部做 32 位对齐读再拆字节）。
#ifndef U8G2_FONTS_FLASH_H
#define U8G2_FONTS_FLASH_H

#include <pgmspace.h>   // 定义 pgm_read_byte（32 位读机制）

#ifdef __GNUC__
#  ifndef U8X8_FONT_SECTION
#    define U8X8_FONT_SECTION(name) __attribute__((section(".irom.text." name)))
#  endif
#  ifndef u8x8_pgm_read
#    define u8x8_pgm_read(adr) pgm_read_byte(adr)
#  endif
#endif

#endif
