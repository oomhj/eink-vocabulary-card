# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project overview

An offline, ultra-low-power **雅思单词卡** (IELTS word-card) for ESP8266 + 2.13" E-Ink display. 当前在用屏为 **HINK-E0213A04-G01（IL3895）**，SSD1680 / 三色屏亦支持（见下文屏幕切换）。Pure offline: no BLE/WiFi/radio is ever enabled. Target hardware is the ESP-12S board in `Schematic_213墨水屏单词卡.png`（同目录另有 .svg）(see `docs/hardware.md` for the full pin map).

**分支**：`main` = 单词卡固件（本文档范围）；`ink-driver-test` = 屏型号探针/实验（已独立为 [eink-driver-test](https://github.com/oomhj/eink-driver-test) 项目，同 `photo-album`，main 上无这些文件）；`daily-calendar` / `photo-album` = 其他 APP（自单词卡基线独立 fork）。

**Firmware status**: `src/word_cards.cpp` is the **ESP8266 firmware** (buildable, in use). `src/word_cards_ref.cpp` is **ESP32 code, reference-only** — the behavioral reference (boot → deep-sleep 60s → refresh a random word → repeat; SW4/RST button for manual refresh). It uses ESP32-only GPIOs/APIs and is excluded from the `esp8266` build via `build_src_filter`.

Key ESP8266 porting facts (see `docs/ink_displays.md` §10 for the driver findings):
- **Screen driver is switchable**: `USE_Z98C` / `USE_SSD1680` in `word_cards.cpp` — `USE_Z98C=1`=三色屏（`GxEPD2_213_Z98c`，无快刷，每次全刷 ~15s）；`USE_SSD1680=1`=SSD1680（`GxEPD2_213_B74`）；两者均 `0`（默认）=HINK-E0213A04-G01 IL3895（`GxEPD2_213_HINK` — 本仓库自定义类 `src/hink_e0213.*`，克隆库 `GxEPD2_213`，仅 VCOM 0x2C=0x18 替代库默认 0xa8，见 `docs/ink_displays.md` §10.1）。各屏均已实物验证；SSD1680 快刷最佳（全刷 1.9s vs IL3895 3.9s）。
- **Flash data reads need `pgm_read_byte`** (ESP8266 flash-mapped region only supports 32-bit access; direct byte reads → LoadStoreError). `src/u8g2_fonts_flash.h` redefines u8g2's `u8x8_pgm_read` to `pgm_read_byte`, so u8g2 fonts in flash work. Any dict/text access in PROGMEM must use `pgm_read_byte`.
- **Single Chinese font** (wqy14, 252KB): the reference's wqy16/wqy14 dual-font scheme doesn't fit the ~1MB `.irom0.text` cap. Flash budget ~94%.

## Commands

```sh
pio run -e esp8266            # build
pio run -e esp8266 -t upload  # flash firmware
pio device monitor            # serial monitor (@74880，对齐 boot ROM；固件亦 74880)
```

> Note: the doc comment in `src/word_cards_ref.cpp` says `pio run -e word-cards`, but the actual PlatformIO environment is named `esp8266` (see `platformio.ini`).

Regenerating the dictionary (see below) is done on the host, not via PlatformIO.

## Dictionary pipeline

`src/dict_ielts.h` is **generated** — never edit it by hand. Editing the word list means editing ECDICT data or the generator, then regenerating:

```sh
python3 tools/gen_dict_h.py --level ielts --out src/dict_ielts.h
```

The generator reads ECDICT's `ecdict.csv` (default location: sibling project `../ECDICT/ecdict.csv`, override with `--csv`). Other levels: `cet4`, `cet6`, `ky`, `gk`. `data/words_demo.txt` is a hand-written sample of the same `word|phonetic|meaning` format.

### Wire format (the contract between generator and firmware)

Each dictionary line is `word|phonetic|meaning`:
- `|` separates fields; fields may never contain a literal `|` (generator replaces it with space)
- Real newlines inside `meaning` are escaped to the literal two characters `\n`; the firmware's `parseEntry()` un-escapes them
- `meaning` is truncated at 191 chars by the parser; `word`/`phonetic` at 31
- The generator sanitizes IPA phonetic glyphs to ASCII approximations (`æ→ae`, `θ→th`, …) because U8g2's fonts cannot render IPA — see `PHONETIC_MAP` in `tools/gen_dict_h.py`

`DICT_WORDS` (the `#define`) and the byte offsets into `dictText[]` both flow from this generated file. Changing a word count or format without regenerating breaks the index.

## Runtime architecture (reference firmware)

The full reference program is one file, `src/word_cards_ref.cpp` (ESP32 — see project overview). It documents the intended behavior and rendering logic that the ESP8266 firmware reproduces; details like the sleep API, NVS, and battery ADC must be re-implemented per `docs/hardware.md` §12. Small RAM footprint (~250KB dict lives in PROGMEM):

- **Boot-time index** — `buildIndex()` scans `dictText` once for `\n` boundaries and stores the starting byte offset of every entry in `idx[]`, enabling O(1) random access to a variable-length line. `wordCount` comes from this scan, not just `DICT_WORDS`.
- **Rendering** — landscape (rotation 3, 250×122). Word + phonetic on top, horizontal divider, Chinese meaning wrapped below (UTF-8 char-aware, stops at y≈98 to avoid the page counter), battery icon top-right, `R<cycle> <round>/<count>` bottom-right. Battery is read + interpolated against `battCurve[]` on every refresh.
- **Fast vs full refresh** — most refreshes are partial (fast, ~0.2s, slight ghosting); every 20th is full (clean, ~4s) via `FULL_EVERY`. `renderCard(n, fast=true)` sets a partial window; the GxEPD2 library forces a full refresh on the first page.
- **Power management** — `esp_light_sleep_start()` with 60s timer + ext0 wakeup on GPIO39. RTC fast/slow memory power domains are disabled (state lives in DRAM, which survives light sleep). After any wake, the button must fully release before sleeping again (`while (digitalRead(PIN_BTN) == LOW)` in `loop()`) so one press = one word.
- **NVS persistence** — round counter, cycle counter, and last word index survive power-off via the `wc` Preferences namespace. Saved every 10 refreshes (`saveCount`) to limit flash wear; `roundCount` wraps to 1 and bumps `cycleCount` when it passes `wordCount`.

## 实际 ESP8266 固件（word_cards.cpp）

当前出货行为（main 分支，已上板）与参考一致，按 `docs/hardware.md` §12 移植：

- **词库访问** — 两趟扫描（不占 20KB 索引 RAM）：`countEntries()` 数词条、`parseNth(n)` 重扫到第 n 条解析（word/phonetic/meaning，`\n` 反转义）。
- **渲染** — 布局同参考（横屏 250×122：词/音标/分隔线/中文释义/电量/页码）；中文统一 wqy14 单字体。
- **刷新** — 快刷为主、每 `FULL_EVERY`=10 次全刷清残影（IL3895 残影积累快，20 太脏）；刷完 `display.hibernate()` 屏深睡。
- **电源管理** — `ESP.deepSleep()` 60s/词；唤醒=整机重启，工作态存 RTC 内存（`State` + magic），断电保持存 LittleFS（`/wc_state.bin`，每 10 次存一次省 flash 磨损）；RST（SW4）手动刷，一次按下=一个词。

## Hardware wiring

Full schematic-derived hardware reference: **`docs/hardware.md`** (display FPC pinout, ESP-12S pin map, buttons, TF card, power tree, battery detection). The board is an **ESP8266 (ESP-12S)** design:

| Signal | ESP8266 pin (per schematic) |
|---|---|
| EPD SCK / SDA(MOSI) | IO14 / IO13 (HW SPI) |
| EPD CS / DC / RST / BUSY | IO15 / IO5 / IO2 / IO4 |
| Battery ADC | ADC (TOUT, 0–1V range)；采样使能 IO12（HIGH 接通分压，≈×1/5.7） |
| Buttons (active-low to GND) | 上键 SW2=IO0（strap，下一个词）、唤醒 SW4=RST（不占 GPIO）；下键已取消 |
| TF card | 已移除（词库在 flash/PROGMEM） |
| Charger / LDO | TP4054 (U8), RT9013-33GB (U3) |

> ⚠️ **11 个 GPIO 全部专用，无富余脚**（IO0–IO5、IO12–IO16：屏 6 个 + 电池采样 IO12 + 上键 IO0 + 深睡唤醒 IO16 + UART0 烧录 IO1/IO3）。新外设只能分时复用 BUSY（IO4，仅刷新期间有效）或外挂扩展芯片，见 `docs/hardware.md` §5。
>
> ✅ **烧录无需断开任何器件**（2026-08 复核）：RST 网络零直流负载（D4/R33/SW4 支路 SW4 断开时开路，另有 C18 隔直滤波），USB-TTL 直接驱动 RST 焊盘即可进下载模式。

> ⚠️ **Firmware is ESP32 reference code**: `word_cards_ref.cpp` uses ESP32-only GPIOs (5/17/16/4, 39, 19, 35) and APIs (`Preferences`, `esp_light_sleep_start`, `esp_random`, `analogReadMilliVolts`). The schematic/platformio.ini target ESP8266, where GPIO17/39/19/35 don't exist — it is **not** meant to compile here. Use it as the behavioral reference when writing new ESP8266 firmware; `docs/hardware.md` §12 has the pin/API porting table.

Build config: 4MB module, `huge_app.csv` partition (needed for the ~247KB dict), LittleFS, `flash_mode=qio` @ 默认 40MHz（26.7MHz 降频已撤回——boot 卡死疑为板子设计问题；如需验证可临时加 `board_build.f_flash = 26700000L`）。Flash 预算 ~94%。Dependencies: `zinggjm/GxEPD2` and `olikraus/U8g2_for_Adafruit_GFX`。
