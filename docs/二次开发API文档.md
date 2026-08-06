# 小智 AI · ElectronBot 人员识别问候方案 — 二次开发 API 文档

> 版本：v2（颜色 + 性别 + 去重问候）｜ 适用硬件：ElectronBot（ESP32‑S3 主控） + Seeed XIAO ESP32S3 Sense（从机）
> 文档生成日期：2026‑08‑04 ｜ 基于源码实际接口整理

---

## 1. 方案概述

本方案在开源项目 **xiaozhi‑esp32** 的基础上，为 `electron-bot` 板扩展了一套**人员识别联动问候**能力：

- **Seeed XIAO ESP32S3 Sense**（从机）承担人脸检测，并通过摄像头画面识别**衣服主色**和**性别**，生成个性化问候。
- **ElectronBot**（主控，ESP32‑S3）通过串口接收通知，生成中文问候语（如"你好蓝色的小哥"），由小智 TTS 朗读，同时驱动头部/手臂舵机做动作。
- 两端**硬件连接方式不变**：仅 Seeed 的 `GPIO43 (TX)` → ElectronBot 的 `GPIO1 (RX)`，115200 8N1 单向 UART。

### 1.1 数据流总览

```
┌─────────────────────────────┐         UART 115200/8N1         ┌──────────────────────────────┐
│  Seeed XIAO ESP32S3 Sense    │   {"event":"person_greeted",    │   ElectronBot (ESP32‑S3)      │
│  (从机, 人脸检测)             │    "color":"blue","gender":     │   (主控, 行为+语音)            │
│                              │     "male",...}                 │                              │
│  OV3660 摄像头                │ ───────────────────────────────▶│  FaceDetectReceiver          │
│     │                        │                                 │     │                        │
│     ▼                        │   {"event":"face_track",...}    │     ▼                        │
│  HumanFaceDetect             │ ───────────────────────────────▶│  BuildGreetingText()         │
│     │                        │   (每帧追踪，可选)               │     │                        │
│     ▼                        │                                 │     ▼                        │
│  HSV 颜色统计 + 宽高比性别      │                                 │  WakeWordInvoke(greeting)    │
│     │                        │                                 │     │  小智 TTS 朗读            │
│     ▼                        │                                 │     ▼                        │
│  uart_notifier_send_*()      │                                 │  QueueAction(ACTION_FACE_    │
│                              │                                 │     GREET) → 舵机动作         │
└─────────────────────────────┘                                 └──────────────────────────────┘
```

### 1.2 关键特性（v2）

| 特性 | 说明 |
|------|------|
| 颜色识别 | 人脸框正下方衣服区域 RGB→HSV 主色统计，支持 10 种颜色 + unknown |
| 性别粗判 | 基于人脸宽高比启发式（非 AI 模型） |
| 去重打招呼 | 同一 `person_id` 30s 冷却；12s 无人到场重置记录 |
| 个性化问候 | 自动生成中文问候语，无需录制音频 |
| 人脸追踪 | 可选，每 100ms 上报一次人脸中心坐标，主控驱动头部跟随 |
| 音频 | 走板载 I2S 扬声器（蓝牙音响功能已移除，见 §9） |
| 显示 | 待机状态栏不再显示时钟（见 §8） |

---

## 2. 目录结构

```
xiaozhi-esp32/
├── main/
│   ├── boards/electron-bot/                 # 主控板代码
│   │   ├── electron_bot.cc                  # 板级初始化（音频 codec / UART 引脚）
│   │   ├── electron_bot_controller.cc       # 行为控制器（接收回调、动作队列、WakeWordInvoke）
│   │   ├── face_detect_receiver.h/.cc        # 【主控 API】UART 接收 + 问候语生成
│   │   ├── bt_a2dp_source.cc/.h              # 蓝牙音响（已禁用：CONFIG_BT_A2DP_ENABLE=n）
│   │   ├── movements.h/.cc                   # 舵机动作库
│   │   └── seeed-sense-face/                 # 【从机】独立 ESP‑IDF 工程
│   │       ├── CMakeLists.txt                # 顶层工程
│   │       ├── partitions.csv                # 8MB 分区表（app0@0x10000, otadata@0xe000）
│   │       ├── sdkconfig / sdkconfig.defaults
│   │       └── main/
│   │           ├── face_detector_main.cpp    # 【从机主程序】检测循环、去重、发送
│   │           ├── uart_notifier.h/.c         # 【从机 API】UART 发送
│   │           ├── color_gender_analyzer.h/.c # 【从机 API】颜色 + 性别分析
│   │           └── idf_component.yml
│   ├── display/lvgl_display/lvgl_display.cc   # LVGL 显示（去除了时钟显示）
│   └── application.h                          # 小智 Application（含 WakeWordInvoke）
└── docs/
    ├── hardware_connection.html              # 接线图 + 协议说明（可视化）
    └── 二次开发API文档.md                     # 本文档
```

> ⚠️ `bt_a2dp_source.*` 文件仍在磁盘上，但 `CONFIG_BT_A2DP_ENABLE=n` 后其内容被条件编译全部排除，**不参与编译**。二次开发无需关心。

---

## 3. 硬件接口

### 3.1 接线表（最少 2 根线）

| # | Seeed XIAO ESP32S3 Sense | 连接 | ElectronBot (ESP32‑S3) | 说明 |
|---|--------------------------|------|------------------------|------|
| 1 | `GPIO43`（UART1 TX） | → | `GPIO1`（UART2 RX） | 人脸事件单向传输，TX→RX 交叉 |
| 2 | `GND` | ⟺ | `GND` | **必须共地**，否则 UART 失败 |
| 3 | `3V3`/`5V`（可选） | → | 主控 3.3V/5V | 建议 Seeed 独立 USB‑C 供电（电流可达 240mA+） |

- 电平：两端均为 3.3V IO，**不可接 5V IO**（会损坏 ESP32‑S3）。
- 接线顺序：先 GND → 再 TX/RX → 最后上电。

### 3.2 UART 参数

| 参数 | 值 |
|------|----|
| 波特率 | `115200` |
| 数据格式 | `8N1`（8 数据位 / 无校验 / 1 停止位） |
| 消息格式 | JSON 行协议，每条以 `\n` 结尾 |
| Seeed TX 引脚 | `GPIO43`（UART1） |
| ElectronBot RX 引脚 | `GPIO1`（UART2，宏 `FACE_UART_RX_PIN`） |
| ElectronBot TX 引脚 | `GPIO_NUM_NC`（主控不回传） |

---

## 4. UART 通信协议（v2）

### 4.1 事件类型

#### 4.1.1 打招呼事件 `person_greeted`（去重后，每条只发一次）

```json
{"event":"person_greeted","color":"blue","gender":"male","count":1,"confidence":0.92,"person_id":3,"face_x":100,"face_y":80,"face_w":80,"face_h":100}
```

| 字段 | 类型 | 取值 | 说明 |
|------|------|------|------|
| `event` | string | `person_greeted` | 固定 |
| `color` | string | `red`/`orange`/`yellow`/`green`/`cyan`/`blue`/`purple`/`white`/`black`/`gray`/`unknown` | 衣服主色（HSV 统计） |
| `gender` | string | `male`/`female`/`unknown` | 人脸宽高比粗判 |
| `count` | int | ≥1 | 画面人脸数 |
| `confidence` | float | 0.0~1.0 | 最高人脸置信度 |
| `person_id` | int | 0~7 | 人脸中心 X 区域编号，用于去重 |
| `face_x/y` | int | 像素 | 人脸框左上角坐标 |
| `face_w/h` | int | 像素 | 人脸框宽高 |

#### 4.1.2 追踪事件 `face_track`（每 ~100ms 一帧，可选）

```json
{"event":"face_track","has_face":1,"face_x":100,"face_y":80,"face_w":80,"face_h":100,"center_x":140,"center_y":130}
```

| 字段 | 类型 | 说明 |
|------|------|------|
| `event` | string | `face_track` |
| `has_face` | int | 0=无人，1=有人 |
| `center_x/y` | int | 人脸中心坐标（无人时 -1） |
| `face_x/y/w/h` | int | 人脸框（无人时 -1/0/0/0） |

> 兼容旧版：`face_detected` 事件也会被主控解析为打招呼（color/gender 置 unknown）。

---

## 5. 从机（Seeed）API

> 源码：`main/boards/electron-bot/seeed-sense-face/main/`

### 5.1 `uart_notifier.h` — UART 发送

```c
// 初始化 UART 通知模块（在 app_main 中调用一次）
void uart_notifier_init(void);

// 发送"打招呼"通知（去重后调用）
void uart_notifier_send_person_greeted(
    int count,            // 画面人脸数
    float confidence,     // 最高置信度 0.0~1.0
    const char *color_name,   // 衣服颜色英文名，如 "blue"（NULL 自动转 "unknown"）
    const char *gender_name,  // 性别英文名 "male"/"female"/"unknown"
    int person_id,        // 去重区域 ID 0~7
    int face_x, int face_y,   // 人脸框左上角
    int face_w, int face_h);  // 人脸框宽高

// 发送"人脸追踪"通知（每帧调用，主控可选订阅）
void uart_notifier_send_face_track(
    int has_face,             // 0/1
    int face_x, int face_y,   // 人脸框左上角（无人 -1）
    int face_w, int face_h,   // 宽高（无人 0）
    int center_x, int center_y); // 中心坐标（无人 -1）
```

- 引脚/波特率由 `uart_notifier.h` 宏定义：`FACE_NOTIFY_UART_PORT=UART_NUM_1`、`FACE_NOTIFY_UART_TX_PIN=GPIO_NUM_43`、`FACE_NOTIFY_UART_BAUD=115200`。

### 5.2 `color_gender_analyzer.h` — 颜色与性别分析

```c
typedef enum {
    COLOR_RED, COLOR_ORANGE, COLOR_YELLOW, COLOR_GREEN, COLOR_CYAN,
    COLOR_BLUE, COLOR_PURPLE, COLOR_WHITE, COLOR_BLACK, COLOR_GRAY, COLOR_UNKNOWN
} cloth_color_t;

typedef enum {
    GENDER_MALE, GENDER_FEMALE, GENDER_UNKNOWN
} gender_t;

// 分析人脸框下方衣服区域主色（纯 C，RGB888 缓冲）
cloth_color_t analyze_cloth_color(
    const uint8_t *rgb_buf,  // RGB888, row-major: R G B R G B ...
    int img_w, int img_h,
    int face_x1, int face_y1, int face_x2, int face_y2); // 人脸框对角坐标

// 根据人脸宽高比粗判性别（ratio>=0.82 → 男）
gender_t estimate_gender(int face_x1, int face_y1, int face_x2, int face_y2);

// 枚举 → 中文名
const char *color_to_chinese(cloth_color_t color);   // "红色".."彩色"
const char *gender_to_chinese(gender_t gender);       // "小哥"/"美女"/"朋友"
```

**颜色判定规则（OpenCV HSV 整数版，H:[0,179] S:[0,255] V:[0,255]）：**

| 颜色 | 条件 |
|------|------|
| 黑 | V < 50 |
| 白 | S < 40 且 V > 180 |
| 灰 | S < 50（其余低饱和） |
| 红 | H ≤ 10 或 H ≥ 160 |
| 橙 | 11 ≤ H ≤ 25 |
| 黄 | 26 ≤ H ≤ 34 |
| 绿 | 35 ≤ H ≤ 85 |
| 青 | 86 ≤ H ≤ 99 |
| 蓝 | 100 ≤ H ≤ 130 |
| 紫 | 131 ≤ H ≤ 159 |

**性别判定：** `ratio = face_w / face_h`；`ratio ≥ 0.82` → `GENDER_MALE`，否则 `GENDER_FEMALE`（启发式，准确率有限）。

### 5.3 `face_detector_main.cpp` — 主循环与可调参数

**编译期宏（修改后需重新编译从机）：**

| 宏 | 默认 | 含义 |
|----|------|------|
| `IMG_W` / `IMG_H` | 320 / 240 | 图像分辨率（QVGA） |
| `GREET_COOLDOWN_MS` | 30000 | 同一 `person_id` 两次打招呼最小间隔 |
| `PRESENCE_TIMEOUT_MS` | 12000 | 超过此时间无人到场，重置所有去重记录 |
| `PERSON_ID_SLOTS` | 8 | 横向位置分区数（person_id 范围 0~7） |
| `TRACK_INTERVAL_MS` | 100 | 追踪消息发送间隔（约 10fps） |

**入口函数：**

```cpp
extern "C" void app_main(void);   // 调用 uart_notifier_init() → camera_init() → 创建 face_detect_task
```

**摄像头引脚：** OV3660（Seeed XIAO ESP32S3 Sense 板载），由 `CAM_PIN_*` 宏定义；检测频率约 10fps（`vTaskDelay(100)`）。

**去重逻辑（在从机端完成）：**
1. 每帧对每个检测到的脸计算 `person_id = center_x * PERSON_ID_SLOTS / IMG_W`。
2. 若 `now - last_greeted[pid] < GREET_COOLDOWN_MS` → 跳过。
3. 否则执行颜色/性别分析并 `uart_notifier_send_person_greeted()`，更新 `last_greeted[pid]`。
4. 每帧 `break` 限制**只打一次招呼**（如需同帧多人，删除该 `break`）。
5. 连续 `PRESENCE_TIMEOUT_MS` 无人 → 清空所有 `last_greeted[]`。

---

## 6. 主控（ElectronBot）API

> 源码：`main/boards/electron-bot/face_detect_receiver.h/.cc` + `electron_bot_controller.cc`

### 6.1 `PersonInfo` 结构体

```cpp
struct PersonInfo {
    int         count;        // 画面人脸数
    float       confidence;   // 最高置信度
    std::string color;        // 衣服颜色英文名
    std::string gender;       // 性别英文名
    int         person_id;    // 去重区域 ID 0~7

    bool        has_face;     // 是否检测到人脸
    int         face_x, face_y;  // 人脸框左上角
    int         face_w, face_h;  // 人脸框宽高
    int         center_x, center_y; // 人脸中心
};
```

### 6.2 `FaceDetectReceiver` 类

```cpp
class FaceDetectReceiver {
public:
    using PersonGreetedCallback =
        std::function<void(const PersonInfo& info, const std::string& greeting_text)>;
    using FaceTrackCallback =
        std::function<void(bool has_face, int center_x, int center_y)>;

    FaceDetectReceiver();
    ~FaceDetectReceiver();

    // 启动 UART 接收任务
    //   on_person_greeted: 识别到新人员回调（已生成中文问候语）
    //   on_face_track:     人脸追踪回调（可选，传 nullptr 关闭追踪）
    void Start(PersonGreetedCallback on_person_greeted,
               FaceTrackCallback on_face_track = nullptr);

    // 停止接收任务（析构自动调用）
    void Stop();

    // ── 静态工具函数（可被二次开发直接调用） ──
    static std::string BuildGreetingText(const std::string& color,
                                         const std::string& gender);
    static const char* ColorToChinese(const std::string& color_en);
    static const char* GenderToChinese(const std::string& gender_en);
};
```

**配置宏（`face_detect_receiver.cc`）：**

| 宏 | 默认 | 含义 |
|----|------|------|
| `FACE_UART_PORT` | `UART_NUM_2` | 主控接收 UART |
| `FACE_UART_RX_PIN` | `GPIO_NUM_1` | 接 Seeed TX；冲突时改此处 |
| `FACE_UART_BAUD` | `115200` | 波特率 |
| `LOCAL_DEBOUNCE_MS` | `500` | 主控端额外防连发 |

### 6.3 问候语生成规则

`BuildGreetingText(color, gender)` 组合逻辑：

```
"你好" + [ColorToChinese(color) + "的"] + GenderToChinese(gender)
```

| color | gender | 输出 |
|-------|--------|------|
| `blue` | `male` | 你好蓝色的小哥 |
| `red` | `female` | 你好红色的美女 |
| `unknown` | `male` | 你好小哥 |
| `unknown` | `unknown` | 你好朋友 |

- `ColorToChinese`：`red→红色`、`orange→橙色`、`yellow→黄色`、`green→绿色`、`cyan→青色`、`blue→蓝色`、`purple→紫色`、`white→白色`、`black→黑色`、`gray→灰色`、`unknown→nullptr`（不加颜色前缀）。
- `GenderToChinese`：`male→小哥`、`female→美女`、`unknown→朋友`。

### 6.4 行为集成（`electron_bot_controller.cc`）

**接收回调示例（已实现）：**

```cpp
face_detect_receiver_.Start(
    [this](const PersonInfo& info, const std::string& greeting_text) {
        pending_greeting_ = greeting_text;                 // 存入共享字符串
        QueueAction(ACTION_FACE_GREET, info.person_id, 800, 0, 0);  // 入队问候动作
    },
    [this](bool has_face, int cx, int cy) {
        OnFaceTrack(has_face, cx, cy);                      // 头部/身体跟随
    });
```

**动作类型枚举（内部使用，不通过 MCP 暴露）：**

| 枚举 | 值 | 含义 |
|------|----|------|
| `ACTION_HOME` | 21 | 复位初始位置 |
| `ACTION_FACE_GREET` | 22 | 显示 😊 表情 → `WakeWordInvoke(问候语)` → 等待 TTS → 恢复 |
| `ACTION_FACE_TRACK_HEAD_LEFT` | 23 | 头部左转 |
| `ACTION_FACE_TRACK_HEAD_RIGHT` | 24 | 头部右转 |
| `ACTION_FACE_TRACK_HEAD_UP` | 25 | 头部上抬 |
| `ACTION_FACE_TRACK_HEAD_DOWN` | 26 | 头部下俯 |
| `ACTION_FACE_TRACK_BODY_LEFT` | 27 | 身体左转 |
| `ACTION_FACE_TRACK_BODY_RIGHT` | 28 | 身体右转 |

**入队函数：**

```cpp
void QueueAction(int action_type, int p1, int p2, int p3, int p4);
```

### 6.5 小智集成 — `WakeWordInvoke`

```cpp
// main/application.h
void WakeWordInvoke(const std::string& wake_word);
```

- 在 `ACTION_FACE_GREET` 动作中调用 `app.WakeWordInvoke(controller->pending_greeting_)`，把问候语作为"唤醒词"发送给小智，**由小智云端 TTS 朗读**，无需本地音频文件。
- 调用后代码 `vTaskDelay(3000)` 等待 TTS 播放（可按实际语速调整）。

---

## 7. 编译与烧录

### 7.1 环境（macOS 实测可用）

```
IDF 版本 : ESP-IDF v5.5.3
IDF 路径 : /Users/jvine/esp/esp-idf-5/.espressif/v5.5.3/esp-idf
Python   : /Users/jvine/.espressif/tools/python/v5.5.3/venv/bin/python
工具链   : /Users/jvine/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin
Ninja    : /Users/jvine/.espressif/tools/ninja/1.12.1
CMake    : /usr/local/bin/cmake
```

**通用环境变量（每次开新 shell 都要设）：**

```bash
export IDF_PATH=/Users/jvine/esp/esp-idf-5/.espressif/v5.5.3/esp-idf
export IDF_TOOLS_PATH=/Users/jvine/.espressif
export IDF_PYTHON_ENV_PATH=/Users/jvine/.espressif/tools/python/v5.5.3/venv
export PATH="/Users/jvine/.espressif/tools/xtensa-esp-elf/esp-14.2.0_20251107/xtensa-esp-elf/bin:/Users/jvine/.espressif/tools/ninja/1.12.1:/usr/local/bin:/Users/jvine/.espressif/tools/python/v5.5.3/venv/bin:$IDF_PATH/tools:$PATH"
```

> 坑提示：`export.sh` 不会自动把工具链加入 PATH，需手动补；若 `idf.py` 报 `ESP_ROM_ELF_DIR` 未定义，先 `source $IDF_PATH/export.sh` 或 `idf.py reconfigure`。

### 7.2 主控（ElectronBot）编译烧录

```bash
cd /Users/jvine/Documents/xiaozhi-esp32
# 改了 sdkconfig 开关或较大改动时，先 fullclean
python $IDF_PATH/tools/idf.py fullclean
python $IDF_PATH/tools/idf.py -p /dev/tty.usbmodemXXXX build flash
```
- 产物：`build/xiaozhi.bin`（约 3.9 MB）。
- 端口会随插拔变号（如 `14501`/`14601`），烧前用 `ls /dev/tty.usbmodem*` 确认。

### 7.3 从机（Seeed）编译烧录

```bash
cd /Users/jvine/Documents/xiaozhi-esp32/main/boards/electron-bot/seeed-sense-face
python $IDF_PATH/tools/idf.py set-target esp32s3   # 首次需要
python $IDF_PATH/tools/idf.py -p /dev/tty.usbmodemXXXX build flash
```
- 这是**独立 ESP‑IDF 工程**，自带 `partitions.csv`（8MB，app0@0x10000）。
- 含 esp‑dl 人脸模型，首次编译较慢（几分钟）。

### 7.4 抓取串口日志（调试用）

```python
import serial, time
s = serial.Serial("/dev/tty.usbmodemXXXX", 115200, timeout=1)
s.setDTR(False); s.setRTS(True); time.sleep(0.1)  # EN 拉低复位
s.setRTS(False)                                    # 释放复位，正常启动
# 之后 s.read(...) 读日志
```
- Seeed 关键日志：`Greet! pid=... color=... gender=...`、`Cloth ROI ...`、`Camera initialized`。
- 主控关键日志：`人员识别通知: pid=...`、`触发小智唤醒：WakeWordInvoke(...)`。

---

## 8. 显示改动（已去除时间）

文件：`main/display/lvgl_display/lvgl_display.cc` 的 `UpdateStatusBar()`。

- **改动前**：设备空闲超过 10 秒后，状态栏跳成时钟 `HH:MM`。
- **改动后**：删除该段逻辑，待机时状态栏保持上一次文本（如唤醒词提示）。
- 电量/静音/网络图标、低电量弹窗等其它显示**不受影响**。
- 二次开发如需恢复时钟，在 `UpdateStatusBar()` 中重新加入 `strftime(time_str, ...)` 逻辑即可。

---

## 9. 蓝牙音响功能（已移除）

- 原功能：`CONFIG_BT_A2DP_ENABLE=y` 时，所有音频经 `BtRoutedAudioCodec` 路由到蓝牙 A2DP 音响，NVS 记忆上次设备自动重连。
- **当前状态**：`CONFIG_BT_A2DP_ENABLE=n`（在 `sdkconfig` 与 `sdkconfig.defaults.esp32s3` 中均已关闭），音频固定走板载 I2S 扬声器。配网页「蓝牙音响」Tab 已删除。
- 源码 `bt_a2dp_source.*` 仍保留但在条件编译外被排除，**不影响编译与运行**。
- 如需恢复：`sdkconfig.defaults.esp32s3` 中设 `CONFIG_BT_A2DP_ENABLE=y` 并重新 fullclean 编译（注意 ESP32‑S3 上 A2DP 路由分支受 `CONFIG_IDF_TARGET_ESP32` 限制，S3 实际仍走板载扬声器）。

---

## 10. 二次开发指南

### 10.1 增加一种新颜色

1. `color_gender_analyzer.h`：`cloth_color_t` 增加枚举值。
2. `color_gender_analyzer.c`：
   - `hsv_to_color()` 增加分支；
   - `color_to_chinese()` 增加中文名。
3. `face_detector_main.cpp`：`color_to_english()` 增加英文映射（用于 JSON）。
4. `face_detect_receiver.cc`：`ColorToChinese()` 增加英文→中文映射（用于问候语）。
5. 重新编译从机 + 主控。

### 10.2 修改问候语格式

编辑 `face_detect_receiver.cc` 的 `BuildGreetingText()`（主控侧，无需重烧从机）。例如改为"欢迎光临，蓝色衣服的帅哥"：

```cpp
std::string text = "欢迎光临，";
if (color_cn) text += std::string(color_cn) + "衣服的";
text += gender_cn;
```

### 10.3 调整打招呼频率 / 去重

- 改从机 `face_detector_main.cpp` 的 `GREET_COOLDOWN_MS`（冷却）或 `PRESENCE_TIMEOUT_MS`（重置）。
- 改 `PERSON_ID_SLOTS` 可调整位置粒度（值越大越细，但相邻区域易误判为不同人）。

### 10.4 更换 UART 引脚

- 从机：`uart_notifier.h` 的 `FACE_NOTIFY_UART_TX_PIN`（默认 GPIO43）。
- 主控：`face_detect_receiver.cc` 的 `FACE_UART_RX_PIN`（默认 GPIO1）。
- 两端需匹配，且引脚未被其它外设占用。

### 10.5 增加新的 UART 事件类型

1. 从机 `uart_notifier.c` 增加 `uart_notifier_send_xxx()` 发送 JSON。
2. 主控 `face_detect_receiver.cc` 的 `ParseMessage()` 增加 `event` 分支，填充 `PersonInfo`。
3. 如需新动作，`electron_bot_controller.cc` 增加 `ACTION_*` 枚举并在 `ActionTask` 处理。

### 10.6 关闭人脸追踪（省 UART 带宽）

调用 `face_detect_receiver_.Start(on_greet, nullptr)` 不传追踪回调即可；从机仍会发 `face_track`，但主控不处理。

---

## 11. 已知限制

1. **性别判定为启发式**：仅基于人脸宽高比（阈值 0.82），准确率有限，不能作为可靠性别识别。
2. **颜色受光照影响大**：HSV 统计对白平衡、环境光敏感，深色/花纹衣服易被判灰/黑。
3. **去重基于位置分区**：两人站同一横向区域会视为同一 `person_id`；快速横移可能绕过冷却。
4. **单帧只打一次招呼**：默认 `break` 限制，多人同时入场只问候一人（见 §5.3）。
5. **TTS 依赖云端**：`WakeWordInvoke` 走小智云端，断网时无法朗读；本地 `vTaskDelay(3000)` 为固定等待，不感知实际播放结束。
6. **蓝牙 A2DP 在 S3 上实际无效**：`BtRoutedAudioCodec` 的蓝牙分支受 `CONFIG_IDF_TARGET_ESP32` 限制，S3 始终走板载扬声器。
7. **端口变号**：USB 串口在 Mac 上随插拔变号，烧录/抓日志前务必确认当前 `tty.usbmodem*`。

---

## 12. 快速参考：二次开发最常改的文件

| 需求 | 改哪个文件 | 是否需要重烧两端 |
|------|-----------|------------------|
| 改问候语文字 | `face_detect_receiver.cc` `BuildGreetingText()` | 仅主控 |
| 加/改颜色或性别映射 | `color_gender_analyzer.*` + 两端英文映射 | 从机 + 主控 |
| 改冷却/重置时间 | `face_detector_main.cpp` 宏 | 仅从机 |
| 换 UART 引脚 | `uart_notifier.h` / `face_detect_receiver.cc` | 两端 |
| 改动作/舵机 | `electron_bot_controller.cc` + `movements.cc` | 仅主控 |
| 恢复时钟显示 | `lvgl_display.cc` `UpdateStatusBar()` | 仅主控 |
| 恢复蓝牙音响 | `sdkconfig.defaults.esp32s3` `CONFIG_BT_A2DP_ENABLE=y` | 仅主控（需 fullclean） |

---

*文档基于仓库当前源码（2026‑08‑04）整理。如接口有更新，请以对应 `.h` 文件注释为准。*
