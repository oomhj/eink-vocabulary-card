# 墨水屏雅思单词卡（ESP8266）

离线、超低功耗的 **雅思单词卡**：ESP8266（ESP-12S）+ 2.13" 墨水屏。纯离线设计——**从不启用 BLE/WiFi/射频**，整机只由电池 + TP4054 充电供电。

## 行为

- 深睡 60 秒自动唤醒，刷一张**随机词卡**（单词 + 音标 + 中文释义 + 电量 + 轮次页码），刷完立即深睡
- RST（唤醒键 SW4）手动刷一张词，一次按下 = 一个词
- 快刷为主、每 10 次全刷清残影；计数/轮次经 RTC 内存 + LittleFS 断电保持
- 词库 5040 条（雅思），存于 flash（PROGMEM），不占 RAM

## 硬件

- 主控：ESP8266（ESP-12S），原理图见 `Schematic_213墨水屏单词卡.png`
- 屏：2.13" 250×122 墨水屏，**当前在用 HINK-E0213A04-G01（IL3895）**；SSD1680 / 三色屏（GDEY0213Z98）亦支持，同一接线、固件宏切换
- 电池：1S 锂电 + TP4054 充电 + IO12 使能的 N-MOS 分压采样（0–1V ADC）
- 引脚：11 个 GPIO 全部专用（屏 SPI×6 + 屏控制×2 + 电池采样 + 上键 + 深睡唤醒 + UART0），无富余脚

## 构建与烧录

[PlatformIO](https://platformio.org/)，4MB flash 模组 + `huge_app.csv` 大分区（词库 ~247KB）：

```sh
pio run -e esp8266            # 编译
pio run -e esp8266 -t upload  # 烧录
pio device monitor            # 串口监视（74880，对齐 boot ROM）
```

烧录需外接 USB-TTL 到 6P 焊盘（无板载 CH340）：GND / 3V3 / GPIO0 / RST / TXD / RXD，esptool/PlatformIO 自动进下载模式，**无需断开任何器件**。

## 词库

`src/dict_ielts.h` 为**生成文件，勿手改**。改词表 = 改 ECDICT 数据或生成器后重新生成：

```sh
python3 tools/gen_dict_h.py --level ielts --out src/dict_ielts.h
# 其他级别：cet4 / cet6 / ky / gk
```

行格式 `word|phonetic|meaning`（`\n` 为字面转义，IPA 已按 U8g2 字体能力转为 ASCII 近似）。

## 屏幕驱动切换

`src/word_cards.cpp` 顶部：

| 宏 | 屏 |
|---|---|
| `USE_Z98C=1` | 三色屏 GDEY0213Z98（无快刷，全刷 ~15s） |
| `USE_SSD1680=1` | SSD1680（`GxEPD2_213_B74`，快刷最佳） |
| 两者均 `0`（默认） | HINK-E0213A04-G01 IL3895（`GxEPD2_213_HINK`，自定义类 `src/hink_e0213.*`，VCOM 0x2C=0x18 实测调参） |

## 分支

- `main` — 单词卡固件（当前在用）
- `ink-driver-test` — 屏型号探针/实验（独立 fork 为 [eink-driver-test](https://github.com/oomhj/eink-driver-test) 项目，同 `photo-album`）
- `photo-album` — 相册（独立 fork 为 [eink-album](https://github.com/oomhj/eink-album)）
- `daily-calendar` — 每日日历（独立 fork 为 [eink-calendar](https://github.com/oomhj/eink-calendar)）

## 文档

- `docs/hardware.md` — 完整硬件参考（引脚、电源、电池检测、移植指引）
- `docs/ink_displays.md` — 各型号墨水屏实测记录（驱动 IC 识别、调参、遗留问题）
- `CLAUDE.md` — 面向 AI 助手的工程说明（构建细节、架构、注意事项）
