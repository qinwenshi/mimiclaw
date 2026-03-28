# ESP32-S3 Claude Notifier — 完整项目知识文档

本文档供全新 Agent 使用，涵盖从 IDF 环境搭建到项目所有细节的完整指南。读完本文档即可从 0 到 1 重建或扩展该项目。

---

## 1. 项目概述

**Claude Notifier** 是一个基于 ESP32-S3 的 BLE 桌面通知设备：

- 通过 BLE 接收上位机（macOS/Python Host Bridge）发来的命令
- 在 1.54 寸 240×240 TFT 屏幕上显示动画角色 + 中文消息
- 通过 WS2812 RGB LED 闪烁反馈
- 通过 MAX98357A I2S 功放播放 PCM 音效
- 通过按钮向上位机发送 BLE 通知（ALLOW / ALLOW_ALL / MUTE / UNMUTE）
- 检测电池电压和 USB 充电状态

---

## 2. 开发环境搭建

### 2.1 安装 ESP-IDF

```bash
# 推荐版本：ESP-IDF v5.3+（项目使用 v5.x API）
git clone --recursive https://github.com/espressif/esp-idf.git ~/esp/esp-idf
cd ~/esp/esp-idf

# 国内网络：通过乐鑫镜像站下载工具链，速度显著提升
IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets ./install.sh esp32s3

# 海外网络：直接安装
# ./install.sh esp32s3

. ./export.sh   # 每次新终端都需要执行
```

### 2.2 编译与烧录

```bash
cd /Users/10124383/projects/esp32-1.54
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/tty.usbmodem* flash monitor
```

### 2.3 项目目录结构

```other
esp32-1.54/
├── main/
│   ├── app_main.c          # 主程序（1495行）：动画/消息/按钮/BLE命令处理
│   ├── ble_server.c        # BLE NimBLE GATT 服务端（382行）
│   ├── ble_server.h        # BLE API 定义
│   ├── sprite_defs.h       # 精灵动画尺寸常量
│   ├── CMakeLists.txt      # 组件注册 + EMBED_FILES
│   ├── *.bin               # 嵌入式资源（动画帧 + GB2312字体）
│   └── *.pcm               # 嵌入式音频（action/completion/start）
├── components/
│   ├── board_sdk/
│   │   ├── include/
│   │   │   ├── board.h     # 所有引脚定义（唯一硬件真相来源）
│   │   │   ├── lcd_st7789.h
│   │   │   ├── led_ws2812.h
│   │   │   └── colors.h
│   │   └── libs/
│   │       ├── lcd_st7789.c
│   │       └── led_ws2812.c
│   └── espressif__led_strip/  # WS2812 外部驱动组件
├── CMakeLists.txt          # 项目根 CMake（set target esp32s3）
├── sdkconfig               # ESP-IDF 完整配置（勿手动改，用 idf.py menuconfig）
├── sdkconfig.defaults      # 关键默认配置（提交到 git）
├── partitions.csv          # Flash 分区表
└── Schematic PDF           # 硬件原理图
```

---

## 3. 硬件配置

### 3.1 主控芯片

| **项目** | **值**                         |
| ------ | ----------------------------- |
| 芯片     | ESP32-S3-WROOM-1(N16R8)       |
| CPU    | Xtensa 双核 LX7 @ 240MHz        |
| PSRAM  | 8MB OPI PSRAM（启用，80MHz）       |
| Flash  | 16MB（自定义分区表）                  |
| USB    | 原生 USB Serial/JTAG（无需 CP2102） |

### 3.2 所有引脚定义（来自 `components/board_sdk/include/board.h`）

#### LCD ST7789（SPI2_HOST）

| **信号**   | **GPIO** | **说明**             |
| -------- | -------- | ------------------ |
| MOSI/SDA | GPIO10   | SPI 数据             |
| SCLK/SCL | GPIO9    | SPI 时钟             |
| CS       | GPIO14   | 片选（低有效）            |
| DC/RS    | GPIO8    | 高=数据，低=命令          |
| RST      | GPIO18   | 复位（低有效）⚠️ 与电池ADC共用 |
| BLK/背光   | GPIO13   | LEDC PWM（高=亮）      |

#### WS2812 RGB LED

| **信号** | **GPIO** | **说明**          |
| ------ | -------- | --------------- |
| DIN    | GPIO48   | RMT 外设驱动，GRB 格式 |

LED 数量：1 个（WS2812B-2020）

#### 按钮（低有效 + 内部上拉）

| **按钮**          | **GPIO** | **功能**                              |
| --------------- | -------- | ----------------------------------- |
| BOOT (SW2)      | GPIO0    | 短按=ALLOW；长按≥3s=配对模式+ALLOW_ALL       |
| VOL_UP          | GPIO40   | 循环切换音量 0→25→50→100→0%，发 MUTE/UNMUTE |
| VOL_DOWN        | GPIO39   | 发 ALLOW                             |
| POWER (SW3 KEY) | GPIO47   | 仅唤醒背光，不发 BLE                        |

按钮去抖：300ms

#### I2S 功放 MAX98357A（I2S0，输出）

| **信号** | **GPIO** | **说明** |
| ------ | -------- | ------ |
| DIN    | GPIO7    | I2S 数据 |
| BCLK   | GPIO15   | 位时钟    |
| LRCLK  | GPIO16   | 左右时钟   |

采样率：16kHz 立体声，16bit

#### I2S 麦克风 INMP441（未在主程序使用）

| **信号** | **GPIO** | **说明** |
| ------ | -------- | ------ |
| WS     | GPIO4    |        |
| SCK    | GPIO5    |        |
| SD     | GPIO6    |        |

#### 电源检测

| **功能**  | **GPIO** | **ADC**  | **说明**                 |
| ------- | -------- | -------- | ---------------------- |
| 电池电压    | GPIO18   | ADC2_CH7 | ⚠️ 与 LCD_RST 共用！分压比1:2 |
| USB充电检测 | GPIO1    | ADC1_CH0 | \>1V = 有USB供电          |

**⚠️ GPIO18 特殊处理**：该引脚同时是 LCD 复位脚和电池 ADC 采样脚。

- 读 ADC 时需先将其配置为 ADC 模式，读完立即切回 GPIO 输出 HIGH
- 否则 LCD 会被复位！代码中每次读电池前后都有 gpio_set_direction + gpio_set_level 操作

电池电压换算：ADC读数(mV) × 2 = 实际电池电压(mV)

电量百分比查表（ADC 读数 mV → 百分比）：

```other
1970→0%, 2062→20%, 2154→40%, 2246→60%, 2338→80%, 2430→100%
```

（线性插值，低于1970按0%，高于2430按100%）

---

## 4. Flash 分区表 (`partitions.csv`)

```other
nvs,      data, nvs,     0x9000,  0x6000,    # 24KB：BLE配对、应用配置
phy_init, data, phy,     0xF000,  0x1000,    # 4KB：RF校准数据
factory,  app,  factory, 0x10000, 0x600000,  # 6MB：主程序（含嵌入资源）
spiffs,   data, spiffs,  0x610000,0x9F0000,  # ~10MB：备用文件系统
```

---

## 5. LCD 显示驱动

### 5.1 ST7789 控制器

- 物理屏：1.54 寸，240×240 像素，RGB565（16bit）
- GRAM：240×320（ST7789 内部），实际用 240×240，无行列偏移
- SPI 时钟：40MHz（`LCD_SPI_CLOCK_HZ`）
- 接口：SPI2_HOST，无 MISO（只写）

### 5.2 初始化流程（`lcd_st7789.c`）

1. 硬件复位：RST 低 10ms → 高 120ms
2. 软件复位（SWRESET），等待 150ms
3. 退出睡眠（SLPOUT），等待 120ms
4. 设置颜色格式：RGB565 (`COLMOD 0x55`)
5. 设置 Gamma、VCOM、Gate、VRH、VDV
6. 帧率：60Hz (`FRCTRL2 0x0F`)
7. 开启显示反转（INVON）
8. 显示开启（DISPON）
9. 初始化背光 LEDC PWM：LEDC_TIMER_0，5kHz，8bit分辨率

### 5.3 主要 API

```other
void lcd_init(void);                                    // 初始化
void lcd_fill_rect(int x, int y, int w, int h, uint16_t color);  // 填充矩形
void lcd_draw_bitmap(int x, int y, int w, int h, const uint16_t *data);  // 绘制位图
void lcd_backlight_set(uint8_t brightness);             // 0=关, 255=最亮
```

`lcd_fill_rect` 和 `lcd_draw_bitmap` 使用分块 SPI 传输（每块 ≤4090 字节），避免超出 DMA 限制。

### 5.4 屏幕布局

```other
┌────────────────────────────────┐ Y=0
│  Top Bar (240×20)               │     消息文字 + 滚动
├────────────────────────────────┤ Y=20
│                                 │
│   Sprite Animation (200×200)    │     居中显示
│         X=20, Y=20              │
│                                 │
├────────────────────────────────┤ Y=220
│  Bottom Bar (240×20)            │     源标签 + 音量图标 + BLE图标
└────────────────────────────────┘ Y=240
```

### 5.5 离屏缓冲区（全局静态）

```other
uint16_t s_scaled_frame[200 * 200];  // 精灵缩放输出
uint16_t s_top_bar[240 * 20];        // 消息栏
uint16_t s_bottom_bar[240 * 20];     // 状态栏
uint16_t s_sx_lut[200];              // X 缩放查找表（128→200，预计算）
uint16_t s_sy_lut[200];              // Y 缩放查找表（128→200，预计算）
```

---

## 6. 动画系统

### 6.1 精灵帧格式（来自 `sprite_defs.h`）

```other
#define SPRITE_FRAME_W        128
#define SPRITE_FRAME_H        128
#define SPRITE_FRAMES_PER_ROW 8           // 每个动画固定 8 帧，循环播放
#define SPRITE_FRAME_BYTES    (128*128*2) // 每帧 32768 字节
#define SPRITE_ROW_BYTES      (8 * SPRITE_FRAME_BYTES)
```

- 每个 bin 文件 = 8 帧连续排列（总大小 = 8 × 32768 = 262144 字节）
- 格式：RGB565，大端序（SPI 直传，无需额外字节交换）
- 显示尺寸：最近邻缩放到 200×200（预计算 LUT，`s_sx_lut[200]` 和 `s_sy_lut[200]`）
- 缩放算法：`s_sx_lut[i] = i * 128 / 200`（integer，非浮点）

### 6.2 动画状态

| **枚举**        | **名称**   | **帧延迟** | **文件**       | **说明**                |
| ------------- | -------- | ------- | ------------ | --------------------- |
| ANIM_IDLE     | idle     | 140ms   | idle.bin     | 默认待机                  |
| ANIM_HAPPY    | happy    | 110ms   | happy.bin    | 任务完成，播放 completion 音效 |
| ANIM_EXCITED  | excited  | 95ms    | excited.bin  | 工具使用/庆祝               |
| ANIM_SLEEPY   | sleepy   | 185ms   | sleepy.bin   | 静音状态                  |
| ANIM_WORKING  | working  | 105ms   | working.bin  | 处理中                   |
| ANIM_ANGRY    | angry    | 95ms    | angry.bin    | 需要审批，播放 action 音效     |
| ANIM_DRAGGING | dragging | 130ms   | dragging.bin | 离线/等待                 |

### 6.3 嵌入式资源访问方式

使用 ESP-IDF `EMBED_FILES` 将二进制文件链接入固件：

```other
// CMakeLists.txt 中声明
EMBED_FILES idle.bin happy.bin ...

// C 代码中访问
extern const uint8_t idle_bin_start[] asm("_binary_idle_bin_start");
extern const uint8_t idle_bin_end[]   asm("_binary_idle_bin_end");

// 宏简化声明
#define DECL_ANIM_BIN(name) \
    extern const uint8_t name##_bin_start[] asm("_binary_" #name "_bin_start"); \
    extern const uint8_t name##_bin_end[]   asm("_binary_" #name "_bin_end")
```

### 6.4 animation_task 主循环

```other
初始化：lcd_fill(RGB565(12, 14, 18))  // 深蓝灰底色清屏
循环：
  battery_update()（每30s或启动时）
  计算背光亮度（根据 idle 时间）
  if 背光=0：skip SPI 渲染，vTaskDelay(500ms)，continue  // 省电
  缩放当前帧到 200×200
  lcd_draw_bitmap 推送精灵区域
  draw_top_bar()  → lcd_draw_bitmap 推送 Top Bar
  draw_bottom_bar() → lcd_draw_bitmap 推送 Bottom Bar
  帧索引 = (帧索引 + 1) % 8
  vTaskDelay(frame_delay_ms)
```

**省电优化**：背光完全熄灭后，跳过所有 SPI 传输，延迟 500ms 而非逐帧延迟，大幅降低功耗。

---

## 7. 中文显示

### 7.1 GB2312 字体文件

- 文件：`gb2312_16.bin`（嵌入，261696 字节）
- 每字符：16×16 像素位图，每行 2 字节 = 32 字节/字符
- 区间：高字节 0xA1-0xF7（区码），低字节 0xA1-0xFE（位码）
- 字符总数：94 区 × 94 位 = 8836 字符（实际约 8178）

字形数据偏移计算：

```other
uint32_t hi = (uint8_t)text[i] - 0xA1;   // 区码 0~93
uint32_t lo = (uint8_t)text[i+1] - 0xA1; // 位码 0~93
uint32_t offset = (hi * 94 + lo) * 32;   // 字形字节偏移
const uint8_t *glyph = gb2312_16_bin_start + offset;
```

### 7.2 ASCII 渲染

- 内置 3×5 像素点阵字体
- 放大 2×：显示为 6×10 像素
- 仅显示可打印 ASCII（0x20~0x7E）

### 7.3 Top Bar 消息渲染

- 最大消息长度：220 字节（GB2312 混合编码）
- 消息宽度 > 232px（LCD_H_RES - 8）时启用**循环滚动**
    - 滚动方式：`shift = (now - start_ms) / 35 % (tw + 24)`，像素级平移
    - 同时绘制两次（偏移 cycle 像素），实现无缝循环
    - 消息宽度 ≤ 232px 时居中显示
- 消息有效期：10 秒后自动清除
- 动画切换时若无有效消息，自动显示动画名称（大写），持续 10s

### 7.4 UI 颜色方案

```other
// Top Bar
bg    = RGB565(18, 22, 30)   // 深蓝灰底
line  = RGB565(52, 60, 78)   // 底部分隔线（最后一行）
fg    = COLOR_WHITE           // 文字白色

// Bottom Bar
bg       = RGB565(18, 22, 30)
top_line = RGB565(52, 60, 78) // 顶部分隔线（第一行）
text_fg  = RGB565(180, 220, 255) // 文字淡蓝色

// Volume bars
active   = RGB565(180, 220, 255) // 亮蓝
dim      = RGB565(55, 65, 85)    // 暗蓝
muted    = RGB565(120, 120, 120) // 灰色

// BLE icon
connected    = RGB565(80, 200, 80)   // 绿
disconnected = RGB565(60, 70, 90)    // 暗蓝灰

// Battery body outline = RGB565(160, 160, 180)
// Battery fill: green RGB(80,200,80) / yellow RGB(220,200,0) / red RGB(220,60,60) / cyan RGB(0,200,220)
```

**颜色字节序**：所有颜色在写入 LCD 缓冲区前需用 `to_be565()` 做字节交换（CPU 小端 → SPI 大端）：

```other
static inline uint16_t to_be565(uint16_t color) {
    return (uint16_t)((color << 8) | (color >> 8));
}
```

---

## 8. WS2812 RGB LED

### 8.1 驱动配置

```other
// RMT 外设，10MHz，GRB 格式
// GPIO48，1个像素
```

### 8.2 LED 效果 API

```other
void led_init(void);
void led_set_color(uint8_t r, uint8_t g, uint8_t b);
void led_set_color32(uint32_t color);             // 0x00GGRRBB 格式
void led_set_hsv(uint16_t h, uint8_t s, uint8_t v);
void led_off(void);
// 注意参数顺序：on_ms/off_ms 在 count 之前！
void led_blink(uint8_t r, uint8_t g, uint8_t b, uint32_t on_ms, uint32_t off_ms, uint32_t count);
void led_breathe(uint8_t r, uint8_t g, uint8_t b, uint32_t period_ms, uint32_t cycles);
void led_rainbow(uint32_t step_ms, uint32_t cycles);
```

所有效果函数均为**阻塞式**，在 led_task 中调用。

### 8.3 各动画对应 LED 效果（通过 `s_led_queue` 发送给 led_task）

| **动画**        | **调用**                           | **效果**    |
| ------------- | -------------------------------- | --------- |
| ANIM_HAPPY    | `led_blink(0,255,0, 100,100, 4)` | 绿色闪烁 ×4   |
| ANIM_ANGRY    | `led_blink(255,0,0, 100,100, 4)` | 红色闪烁 ×4   |
| ANIM_DRAGGING | `led_blink(255,0,0, 300,200, 4)` | 红色慢速脉冲 ×4 |
| ANIM_SLEEPY   | `led_blink(0,0,255, 500,100, 4)` | 蓝色慢闪 ×4   |
| 其他            | 无                                | 无 LED 效果  |

注意：每次调用 `set_current_anim()` 都会向队列发送（即使动画未变化），所以重复设置同一状态也会触发 LED 效果。

---

## 9. 音频系统

### 9.1 I2S 配置

```other
// I2S0，标准模式，主机
// 采样率：16000 Hz，立体声，16bit
// GPIO7=DIN, GPIO15=BCLK, GPIO16=LRCLK
```

### 9.2 嵌入式 PCM 音频文件

| **文件**         | **符号**               | **触发时机**         |
| -------------- | -------------------- | ---------------- |
| start.pcm      | start_pcm_start      | 设备启动时            |
| completion.pcm | completion_pcm_start | ANIM_HAPPY（任务完成） |
| action.pcm     | action_pcm_start     | ANIM_ANGRY（需审批）  |

格式：16kHz，立体声，16bit signed PCM（与 I2S 配置一致）

### 9.3 音量管理

音量等级 0-3 对应的移位量（右移 = 降低幅度）：

```other
static const int k_vol_shift[4] = {
    0,   // 0: MUTE（实际通过跳过播放实现）
    2,   // 1: 25%（>>2 = ÷4）
    1,   // 2: 50%（>>1 = ÷2）默认
    0    // 3: 100%（不移位）
};
```

audio_task 播放流程：

1. 从 `s_audio_queue`（深度1，可覆写）接收 `audio_clip_t`
2. 快照当前音量（防止播放中途变化）
3. 按块（≤4096字节）缩放 int16_t 样本后写入 I2S
4. 音量为0时跳过播放

### 9.4 音量持久化（NVS）

```other
// 命名空间: "notifier"，键: "vol_level"，类型: uint8_t
// 启动时读取，按钮调整时写入
// 无效值（>3）默认为 2（50%）
```

---

## 10. BLE 通信协议

### 10.1 BLE 栈配置

- 仅使用 NimBLE（ESP32-S3 不支持 Classic BT）
- `sdkconfig` 关键配置：

```other
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_NVS_PERSIST=y   # 配对信息持久化
```

### 10.2 GATT 服务定义

```other
Service UUID:  0xABCD（主服务）
  Char 0x1234: WRITE | WRITE_NO_RSP  （host → device，命令通道，最大300字节）
  Char 0x5678: NOTIFY                （device → host，事件通知）
```

### 10.3 设备名称

格式：`Claude-XXXXXX`（XXXXXX = MAC地址后3字节十六进制大写）

示例：`Claude-A1B2C3`

在 `ble_server_on_sync()` 回调中通过 MAC 地址动态设置。

### 10.4 广播参数

| **模式** | **广播间隔**           | **触发条件**    |
| ------ | ------------------ | ----------- |
| 配对模式   | 100ms（160×0.625ms） | BOOT 长按 ≥3s |
| 正常模式   | 200ms（320×0.625ms） | 默认/断连后      |

安全配置：

- `sm_io_cap = BLE_SM_IO_CAP_NO_IO`（无显示/键盘）
- `sm_bonding = 1`（启用绑定）
- `sm_mitm = 0`（不要求 MITM 保护）
- `sm_sc = 1`（启用 Secure Connections）
- 重复配对时自动删除旧绑定并重试（`BLE_GAP_REPEAT_PAIRING_RETRY`）

### 10.5 Host → Device 命令（写入 Char 0x1234）

命令格式：ASCII 文本，以 `\n` 结尾（解析时会 trim）

| **命令** | **格式**            | **效果**                     |
| ------ | ----------------- | -------------------------- |
| 设置动画   | `ANIM <state>`    | 切换动画状态（多种别名）               |
|        | `EMOTION <state>` | 同上                         |
|        | `STATE <state>`   | 同上                         |
|        | `<state>`         | 直接状态名                      |
| 设置来源   | `SRC <name>`      | 设置底栏来源标签（最大20字符，自动大写）      |
| 设置消息   | `MSG <text>`      | 设置顶栏消息（最大220字节，支持GB2312中文） |
| 审批请求   | `APPROVE <quest>` | 切到 ANGRY 动画，等待用户按键确认       |
| 心跳     | `PING`            | 静默处理，不记录日志（ODEC-164348修复）  |
| 帮助     | `HELP`            | 返回命令列表（通过日志输出）             |

**有效的 `<state>` 值**（不区分大小写）：

```other
idle, happy, love, excited, celebrate, sleepy, snoring,
working, angry, surprised, shy, dragging, completed, stop,
needsattention, needs_attention, askuserquestion, exitplanmode,
tool_use, pendingtooluse, offline, unknown
```

状态到动画的映射（按代码 `lookup_anim()` 顺序）：

| **状态关键字**                                                              | **动画**        | **音效**     |
| ---------------------------------------------------------------------- | ------------- | ---------- |
| `idle`                                                                 | ANIM_IDLE     | 无          |
| `happy`, `love`                                                        | ANIM_HAPPY    | completion |
| `excited`, `celebrate`                                                 | ANIM_EXCITED  | 无          |
| `sleepy`, `snoring`                                                    | ANIM_SLEEPY   | 无          |
| `working`                                                              | ANIM_WORKING  | 无          |
| `angry`, `surprised`, `shy`                                            | ANIM_ANGRY    | action     |
| `dragging`                                                             | ANIM_DRAGGING | 无          |
| `completed`, `stop`                                                    | ANIM_HAPPY    | completion |
| `needsattention`, `needs_attention`, `askuserquestion`, `exitplanmode` | ANIM_ANGRY    | action     |
| `tool_use`, `pendingtooluse`                                           | ANIM_EXCITED  | 无          |
| `offline`, `unknown`                                                   | ANIM_DRAGGING | 无          |

⚠️ 易错点：`celebrate` → EXCITED（不是 HAPPY）；`surprised/shy` → ANGRY（不是 EXCITED）；`tool_use` → EXCITED（不是 WORKING）。

### 10.6 Device → Host 通知（Char 0x5678 NOTIFY）

| **事件**    | **字节串**                | **触发条件**              |
| --------- | ---------------------- | --------------------- |
| ALLOW     | `"ALLOW\n"` (6字节)      | VOL_DOWN 按下 或 BOOT 短按 |
| ALLOW_ALL | `"ALLOW_ALL\n"` (10字节) | BOOT 长按 ≥3s           |
| MUTE      | `"MUTE\n"` (5字节)       | VOL_UP 切换到音量0         |
| UNMUTE    | `"UNMUTE\n"` (7字节)     | VOL_UP 切换到非0音量        |

调用方式：

```other
ble_server_notify("ALLOW\n", 6);
```

---

## 11. 按钮逻辑详解（`monitor_task`）

```other
每 10ms 轮询 GPIO 电平（内部上拉，低有效）：
  检测下降沿 → 记录按下时刻
  检测上升沿（松开）→ 计算持续时间：
    BOOT: 持续 ≥ 3000ms → 配对模式 + ALLOW_ALL
          持续 < 3000ms → ALLOW
    VOL_UP:  循环切换音量等级，发 MUTE 或 UNMUTE
    VOL_DOWN: 发 ALLOW
    POWER:   唤醒背光（更新 s_last_activity_ms）
```

去抖：连续 300ms 后才确认按键状态。

---

## 12. 电源管理

### 12.1 背光自动熄屏

- 空闲超时：5分钟（`BACKLIGHT_IDLE_TIMEOUT_MS = 5 * 60 * 1000`）
- 超时后渐暗：30秒内从最亮逐渐降到0（线性）
- 任何活动（按键/BLE命令）重置 `s_last_activity_ms`
- `BTN_POWER_PIN` 只唤醒背光，不发 BLE

### 12.2 动态功耗管理

```other
sdkconfig:
  CONFIG_PM_ENABLE=y
  CONFIG_FREERTOS_USE_TICKLESS_IDLE=y  # 空闲时降频
```

WiFi 完全禁用（节省约 20-50mA）。

---

## 13. FreeRTOS 任务架构

| **任务**         | **栈大小**  | **优先级** | **职责**                     |
| -------------- | -------- | ------- | -------------------------- |
| animation_task | 4096B    | 5       | 帧缩放/渲染/LCD推送/背光控制/电池检测     |
| command_task   | 2048B    | 4       | USB Serial/JTAG stdin 命令解析 |
| monitor_task   | 2048B    | 5       | 按钮轮询 + BLE 通知发送            |
| audio_task     | 2048B    | 6       | I2S PCM 播放（最高优先级保证音质）      |
| led_task       | 2048B    | 5       | WS2812 LED 效果              |
| BLE host task  | NimBLE内部 | \-      | NimBLE 协议栈                 |

### 13.1 队列

```other
QueueHandle_t s_led_queue;    // 发送给 led_task，anim_id_t，深度5
QueueHandle_t s_audio_queue;  // 发送给 audio_task，audio_clip_t，深度1（xQueueOverwrite）
```

### 13.2 互斥保护

```other
portMUX_TYPE s_state_lock;    // 保护：anim/message/volume/activity/approval
portMUX_TYPE s_conn_lock;     // (ble_server.c) 保护 BLE 连接句柄
```

---

## 14. NVS 使用

命名空间：`"notifier"`

| **键**       | **类型** | **默认值** | **说明**         |
| ----------- | ------ | ------- | -------------- |
| `vol_level` | u8     | 2       | 音量等级 0-3，启动时读取 |

BLE 配对数据由 NimBLE 自动存储在 NVS（无需手动管理）。

---

## 15. Bottom Bar 状态图标

### 电池图标（左侧，14×7 像素）

颜色逻辑：

- 充电中（USB 检测到）：青色
- 电量 >60%：绿色
- 电量 20-60%：黄色
- 电量 <20%：红色

每 30 秒在 animation_task 中更新一次。

### 音量图标（右侧，信号柱样式）

4 根柱子，宽 1px，高度分别 2/4/6/7 像素：

- 音量0（静音）：全暗
- 音量1（25%）：1根亮
- 音量2（50%）：2根亮
- 音量3（100%）：4根亮

### BLE 状态图标（最右，8×7 像素）

- 已连接：绿色
- 未连接：暗色

### 来源标签（中间）

- 来自 `SRC <name>` 命令，最大 20 字符
- **实际显示格式**：`"SOURCE: %s"`（自动带前缀）
- 默认值：`"SOURCE: CLAUDE"`（启动时 source 默认为 "CLAUDE"）
- 若 SRC 命令内容清理后为空，显示 `"SOURCE: UNKNOWN"`
- 宽度超出可用区域时 ping-pong 滚动，周期 3s/方向（基于绝对时钟 `now_ms() % 6000`）

---

## 16. app_main() 启动顺序

严格按此顺序执行，顺序错误会导致硬件冲突（GPIO18 问题）：

```other
1. nvs_flash_init()          // NVS 初始化（若版本不符则擦除重建）
2. nvs_load_volume()         // 从 NVS 读取音量（默认2）
3. lcd_init(&lcd_cfg)        // LCD 初始化（rotation=LCD_ROTATE_0, backlight=255）
                             // ⚠️ 初始化时拉高 GPIO18（RST→HIGH）
4. s_last_activity_ms=now    // 启动视为活动，保持背光
5. battery_adc_init()        // ⚠️ 必须在 lcd_init 之后！GPIO18 此时稳定为 HIGH
6. led_init() + led_task     // WS2812 RMT 初始化
7. audio_init() + audio_task // I2S 初始化
8. audio_play_async(start.pcm) // 播放启动音效
9. ble_server_set_callback() + ble_server_init() // NimBLE 初始化
10. monitor_task             // 按钮监控任务
11. usb_serial_jtag_driver_install() // USB JTAG 驱动（若未安装）
12. set_source_label("CLAUDE") // 设置默认来源标签
13. set_message_bytes(NULL,0)  // 清空消息
14. scale_lut_init()           // 预计算缩放 LUT
15. animation_task + command_task // 启动渲染和命令任务
```

## 17. 本地调试（command_task / USB JTAG）

`command_task` 监听 USB Serial/JTAG 输入，支持与 BLE **完全相同的命令集**，用于本地调试无需上位机连接：

```bash
# 连接后在串口发送（idf.py monitor 或 screen /dev/tty.usbmodem*）：
ANIM happy
SRC TEST
MSG 你好世界
PING
```

- 先尝试 `usb_serial_jtag_read_bytes()`，失败降级到 `fgets(stdin)`
- 支持退格键（0x08/0x7F）
- 每次读超时 20ms，避免阻塞其他任务

## 18. 上位机 Host Bridge（macOS）

Host Bridge 通过 BLE 连接设备并发送命令。通信流程：

```other
上位机 Python/Swift 进程
  → 扫描 BLE 设备（名称以 "Claude-" 开头）
  → 连接
  → 订阅 Char 0x5678 通知
  → 写入 Char 0x1234 发送命令

设备处理命令后：
  → 可通过 Char 0x5678 发送 ALLOW/MUTE 等事件回上位机
```

上位机识别设备方式：扫描广播包中服务 UUID `0xABCD`，或名称前缀 `Claude-`。

---

## 19. 典型问题与调试

### GPIO18 ADC/LCD_RST 竞争

**症状**：读电池后屏幕重置
**原因**：GPIO18 同时是 LCD RST 和 ADC 采样脚
**解决**：读 ADC 前配置为 ADC 模式，读后立即切为 GPIO OUTPUT HIGH

### 背光不亮

检查：`lcd_backlight_set(255)` 是否被调用，LEDC_TIMER_0 是否初始化，GPIO13 是否正确

### BLE 无法连接

- 确认 `CONFIG_BT_NIMBLE_ENABLED=y` 在 sdkconfig 中
- 确认没有开启 `CONFIG_BT_CLASSIC_ENABLED`
- BOOT 长按 ≥3s 进入配对模式（广播间隔缩短到 100ms）

### 音频噪音/失真

- 检查 I2S 时钟配置（16kHz 必须与 PCM 文件采样率一致）
- 确认 MAX98357 GAIN 引脚悬空（默认 9dB）

### 内存不足

- 大缓冲区（如 `s_scaled_frame[200×200×2=80000字节]`）使用全局静态分配
- 避免在任务栈中声明大数组
- PSRAM 可用，但需 `heap_caps_malloc(size, MALLOC_CAP_SPIRAM)` 显式分配

---

## 20. sdkconfig 关键配置摘要

```other
CONFIG_IDF_TARGET="esp32s3"
CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y
CONFIG_PARTITION_TABLE_CUSTOM=y
CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"

# PSRAM
CONFIG_ESP32S3_SPIRAM_SUPPORT=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y

# BLE (NimBLE only)
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_NVS_PERSIST=y
CONFIG_BT_CLASSIC_ENABLED=n

# 电源管理
CONFIG_PM_ENABLE=y

# USB Serial/JTAG
CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y
```

---

## 21. 从零开始创建同类项目的步骤

1. `idf.py create-project myproject && cd myproject`
2. `idf.py set-target esp32s3`
3. 创建 `components/board_sdk/`，复制 board.h（引脚定义）
4. 实现 LCD 驱动：SPI2_HOST + ST7789 初始化序列 + LEDC 背光
5. 实现 WS2812 驱动：RMT 外设，GRB，1个像素
6. 实现 I2S 音频：`driver/i2s_std.h`，16kHz，GPIO7/15/16
7. 实现 BLE：NimBLE，服务 0xABCD，两个特征值
8. 在 `CMakeLists.txt` 中 `EMBED_FILES` 嵌入 bin/pcm 资源
9. 自定义 `partitions.csv`（16MB Flash 布局）
10. 实现 FreeRTOS 多任务架构（animation/audio/led/monitor/ble）
11. 实现 NVS 持久化（音量等配置）

---

*本文档由 Claude Code 自动生成，描述截止 git commit b4372ee（2024年，main 分支）。*

