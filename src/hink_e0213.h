// Custom panel class: IL3895 @ 250x122（2.13"，HINK-E0213A04-G01 实物屏）
// 克隆自 GxEPD2 库 GxEPD2_213（GDE0213B1/IL3895，250x122），命令集/时序/LUT 完全一致，
// 唯一差异：0x2C VCOM 设置值改为可调 static vcomConfig（默认 0x18，库默认 0xa8）。
// 原因：HINK-E0213A04-G01 实物屏调试结论（2026-08，epd_driver_probe 实测）——
//       VCOM=0x18 对比度/残影表现优于库默认 0xa8。
// 库内 _InitDisplay() 为 private 且每次全刷/快刷前都会重写 0x2C，
// 外部写 0x2C 会被覆盖，故只能克隆类改默认值（同 src/uc8151d_250.* 做法）。
//
// Original upstream: GxEPD2 v1.6.9 (Jean-Marc Zingg, https://github.com/ZinggJM/GxEPD2)
// Controller: IL3895 : http://www.e-paper-display.com/download_detail/downloadsId=538.html

#ifndef _GxEPD2_213_HINK_H_
#define _GxEPD2_213_HINK_H_

#include <GxEPD2_EPD.h>

class GxEPD2_213_HINK : public GxEPD2_EPD
{
  public:
    // attributes
    static const uint16_t WIDTH = 128;
    static const uint16_t WIDTH_VISIBLE = 122;
    static const uint16_t HEIGHT = 250;
    static const GxEPD2::Panel panel = GxEPD2::GDE0213B1;
    static const bool hasColor = false;
    static const bool hasPartialUpdate = true;
    static const bool hasFastPartialUpdate = true;
    static const uint16_t power_on_time = 80; // ms, e.g. 72961us
    static const uint16_t power_off_time = 140; // ms, e.g. 135839us
    static const uint16_t full_refresh_time = 4000; // ms, e.g. 3883686us
    static const uint16_t partial_refresh_time = 300; // ms, e.g. 268173us
    // 0x2C VCOM 设置：HINK-E0213A04-G01 实测 0x18（库默认 0xa8 对比度差）
    static uint8_t vcomConfig;
    // constructor
    GxEPD2_213_HINK(int16_t cs, int16_t dc, int16_t rst, int16_t busy);
    // methods (virtual)
    //  Support for Bitmaps (Sprites) to Controller Buffer and to Screen
    void clearScreen(uint8_t value = 0xFF); // init controller memory and screen (default white)
    void writeScreenBuffer(uint8_t value = 0xFF); // init controller memory (default white)
    // write to controller memory, without screen refresh; x and w should be multiple of 8
    void writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImageToPrevious(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                        int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePartToPrevious(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                        int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    // write sprite of native data to controller memory, without screen refresh; x and w should be multiple of 8
    void writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    // write to controller memory, with screen refresh; x and w should be multiple of 8
    void drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    // write sprite of native data to controller memory, with screen refresh; x and w should be multiple of 8
    void drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void refresh(bool partial_update_mode = false); // screen refresh from controller memory to full screen
    void refresh(int16_t x, int16_t y, int16_t w, int16_t h); // screen refresh from controller memory, partial screen
    void powerOff(); // turns off generation of panel driving voltages, avoids screen fading over time
    void hibernate(); // turns powerOff() and sets controller to deep sleep for minimum power use, ONLY if wakeable by RST (rst >= 0)
  private:
    void _writeScreenBuffer(uint8_t value);
    void _writeImage(uint8_t command, const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void _writeImagePart(uint8_t command, const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                         int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void _setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void _setRamEntryWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void _setRamArea(uint16_t xs, uint16_t xe, uint16_t ys, uint16_t ye);
    void _setRamPointer(uint16_t x, uint16_t y);
    void _PowerOn();
    void _PowerOff();
    void _InitDisplay();
    void _Init_Full();
    void _Init_Part();
    void _Update_Full();
    void _Update_Part();
  private:
    static const uint8_t LUTDefault_part[];
    static const uint8_t LUTDefault_full[];
};

#endif
