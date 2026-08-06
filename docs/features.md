# ElectronBot 功能文档

> 基于 xiaozhi-esp32 v2.2.4，ElectronBot 开发板
>
> 最后更新：2026-04-27

---

## 目录

1. [系统概览](#1-系统概览)
2. [硬件配置](#2-硬件配置)
3. [人脸检测与个性化问候](#3-人脸检测与个性化问候)
4. [蓝牙音响功能](#4-蓝牙音响功能)
5. [WiFi 配网页面](#5-wifi-配网页面)
6. [硬件接线图](#6-硬件接线图)
7. [文件变更记录](#7-文件变更记录)

---

## 1. 系统概览

### 1.1 硬件平台

| 项目 | 规格 |
|------|------|
| 主控 | ESP32-S3 (Xtensa LX7 @ 240MHz) |
| 内存 | 8MB PSRAM |
| 存储 | 4MB Flash |
| 语音采样 | 16kHz (MIC) / 24kHz (SPK output) |
| 无线 | WiFi + BLE |
| 显示屏 | 240×240 SPI LCD |

### 1.2 架构图

```
┌─────────────────────────────────────────────────────────────────┐
│                      ESP32-S3 ElectronBot                        │
│                                                                  │
│  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐     │
│  │  WiFi/BLE    │     │   语音引擎    │     │   动作控制    │     │
│  │  配网/通信    │     │  ASR/TTS/Wake│     │  舵机/PWM    │     │
│  └──────┬───────┘     └──────┬───────┘     └──────────────┘     │
│         │                    │                                  │
│         │  I2S               │ I2S                              │
│         ↓                    ↓                                  │
│  ┌──────────────┐     ┌──────────────┐     ┌──────────────┐     │
│  │  蓝牙音响路由  │     │  I2S Codec   │     │   Seeed Sense │     │
│  │  (A2DP)      │     │  MEMS MIC    │     │  (人脸检测)   │     │
│  └──────────────┘     └──────────────┘     └──────────────┘     │
│         │                    │                    │              │
│  ┌──────┴────────────────────┴────────────────────┴─────────┐   │
│  │                      GPIO 引脚                            │   │
│  └───────────────────────────────────────────────────────────┘   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. 硬件配置

### 2.1 GPIO 引脚定义

#### 音频相关

| GPIO | 功能 | 方向 | 说明 |
|------|------|------|------|
| GPIO17 | SPK DOUT | 输出 | I2S 数据输出 → 扬声器 |
| GPIO18 | SPK BCLK | 输出 | I2S 位时钟 |
| GPIO8 | SPK LRCK | 输出 | I2S 字选择 |
| GPIO40 | MIC SCK | 输入 | I2S MIC 时钟 |
| GPIO42 | MIC WS | 输入 | I2S MIC 字选择 |
| GPIO41 | MIC DIN | 输入 | I2S MIC 数据输入 |

#### 舵机控制

| GPIO | 功能 | 说明 |
|------|------|------|
| GPIO5 | 右臂旋转 (Pitch) | PWM 输出 |
| GPIO6 | 身体 (Body) | PWM 输出 |
| GPIO7 | 左臂旋转 (Pitch) | PWM 输出 |
| GPIO16 | 头部 (Head) | PWM 输出 |

#### 显示屏 (SPI)

| GPIO | 功能 |
|------|------|
| GPIO9 | Reset |
| GPIO10 | MOSI |
| GPIO11 | SCLK |
| GPIO12 | CS |
| GPIO13 | DC |

#### 其他

| GPIO | 功能 | 说明 |
|------|------|------|
| GPIO0 | Boot 按钮 | 输入上拉 |
| GPIO14 | 充电检测 | ADC 输入 |
| GPIO46 | 屏幕背光 | 输出 |
| GPIO43 | UART1 TX | Seeed Sense 通信 |
| GPIO1 | UART1 RX | (预留) |
| GPIO2 | UART1 RX | 蓝牙桥接 IP 接收 |

### 2.2 音频参数

```
输入采样率：16000 Hz
输出采样率：24000 Hz
I2S 格式：Standard I2S, 32bit, Mono
ADC：MEMS 数字麦克风 (I2S 输入)
DAC：板载功放 (I2S 输出)
```

---

## 3. 人脸检测与个性化问候

### 3.1 功能描述

当 ElectronBot 检测到人脸时：
1. **个性化问候**：根据检测到的人脸特征（衣服颜色、性别）生成个性化问候语，通过 TTS 朗读
2. **人脸追踪**：持续跟踪人脸位置，头部跟随人脸左右/上下，身体跟随大范围左右移动

### 3.2 硬件连接

Seeed XIAO ESP32S3 Sense 作为独立人脸检测从机，通过 UART 与主控通信：

```
ElectronBot ESP32-S3          Seeed Sense
─────────────────────────       ───────────
GPIO43 (UART1 TX)  ──UART──→  GPIO6 (UART RX)
GND                 ──GND──→  GND
(115200 baud, 8N1)
```

### 3.3 通信协议 (v2)

#### 主控 → Seeed

```json
{"cmd": "start"}
{"cmd": "stop"}
```

#### Seeed → 主控

**打招呼事件**（满足条件时发送一次）：

```json
{
  "event": "person_greeted",
  "color": "blue",
  "gender": "male",
  "person_id": 1,
  "count": 1,
  "confidence": 0.92,
  "face_x": 100,
  "face_y": 80,
  "face_w": 80,
  "face_h": 100
}
```

**位置追踪事件**（每帧持续发送，约 10fps）：

```json
{
  "event": "face_track",
  "has_face": 1,
  "face_x": 100,
  "face_y": 80,
  "face_w": 80,
  "face_h": 100,
  "center_x": 140,
  "center_y": 130
}
```

**字段说明：**

| 字段 | 类型 | 说明 |
|------|------|------|
| event | string | 事件类型：`person_greeted` 或 `face_track` |
| color | string | 衣服主色（greeted 事件）：`red`, `blue`, `green` 等 |
| gender | string | 性别判断（greeted 事件）：`male`, `female`, `unknown` |
| person_id | int | 人脸位置区域 ID（0~7），用于去重 |
| count | int | 画面中人脸数量 |
| confidence | float | 最高置信度 |
| has_face | int | 是否检测到人脸（0/1） |
| face_x/y | int | 人脸框左上角坐标 |
| face_w/h | int | 人脸框宽高 |
| center_x/y | int | 人脸中心坐标（图像坐标系 0~319 / 0~239） |

### 3.4 个性化问候规则

```
颜色 + 性别 → 问候语模板：

蓝色 + 男性 → "你好蓝色衣服的小哥"
蓝色 + 女性 → "你好蓝色衣服的小姐姐"
红色 + 男性 → "你好红色衣服的小哥"
...
黑色 + 男性 → "你好黑色衣服的朋友"

如果没有检测到特征 → 默认问候："你好呀"
```

### 3.5 人脸追踪机制

#### 追踪逻辑

```
图像坐标系（320×240）：
  - X: 0 (左) ←────────────────────→ 319 (右)
  - Y: 0 (上) ←────────────────────→ 239 (下)
  - 中心: (160, 120)
```

#### 追踪策略

| 追踪维度 | 补偿方式 | 最大偏移 |
|---------|---------|---------|
| 人脸偏左/右 | 头部左右转动优先 | 头部 ±30° |
| 人脸偏左/右（大范围）| 身体左右转动（头部到极限时）| 身体 ±45° |
| 人脸偏上/下 | 头部上下运动 | 头部 ±15° |

#### 死区与频率

- **死区**：中心 ±15 像素内不动作，避免抖动
- **追踪频率**：约 20fps（50ms 间隔）
- **头部步进**：每次移动 2°
- **身体步进**：每次移动 5°
- **无人超时**：3 秒无人脸自动回正

#### 追踪流程

1. 接收 `face_track` 事件（10fps）
2. 计算人脸中心与图像中心的偏移
3. 判断是否在死区外
4. 头部优先补偿水平偏移
5. 头部补偿垂直偏移
6. 头部到极限时启用身体转动

### 3.5 去重机制

- **冷却时间**：同一 person_id 检测后 30 秒内不重复问候
- **超时重置**：12 秒内未检测到任何人脸时，重置所有人员状态
- **防抖**：新检测与上次问候间隔需超过 3 秒

### 3.6 关键代码文件

| 文件 | 说明 |
|------|------|
| `face_detect_receiver.cc/h` | UART 协议解析，PersonInfo + 回调 |
| `color_gender_analyzer.c/h` | HSV 颜色分析 + 宽高比性别判断 |
| `uart_notifier.c/h` | Seeed 端 UART 发送 |
| `face_detector_main.cpp` | Seeed 主程序 |

---

## 4. 蓝牙音响功能

### 4.1 功能描述

通过 ESP32 原版开发板作为蓝牙桥接，实现 A2DP 音频播放，让 ElectronBot 可以连接任意蓝牙音响。

### 4.2 系统架构

```
┌─────────────────────────┐         ┌─────────────────────────┐
│    ESP32-S3 主控         │         │    ESP32 Bridge         │
│   (ElectronBot)          │   WiFi   │   (Classic BT)          │
│                          │   TCP    │                          │
│  配网页面 /bt/* API  ──→│────────→│  HTTP Server (:80)     │
│                          │         │         ↓               │
│  I2S SPK Out ───────────│────────→│  I2S Slave In           │
│  24kHz mono              │         │         ↓               │
│                          │         │  44.1kHz 重采样          │
│                          │  UART   │         ↓               │
│  UART1 RX ←──────────────│─────────│  UART2 TX               │
│  (GPIO2)                 │ 115200  │  (GPIO4)                │
└─────────────────────────┘         └───────────┬─────────────┘
                                                  │ A2DP Source
                                                  ↓
                                             🔊 蓝牙音响
```

### 4.3 硬件接线

| 功能 | ESP32-S3 | 方向 | ESP32 Bridge | 说明 |
|------|----------|------|--------------|------|
| I²S 数据 | GPIO17 (DOUT) | → | GPIO32 (DIN) | 音频数据 |
| I²S 时钟 | GPIO18 (BCLK) | → | GPIO26 (BCLK) | 位时钟 |
| I²S 字选 | GPIO8 (LRCK) | → | GPIO27 (WS) | 左右声道 |
| UART 广播 | GPIO2 (RX) | ← | GPIO4 (TX) | Bridge IP 通知 |
| 共地 | GND | — | GND | 必须共地 |

### 4.4 工作流程

1. **Bridge 上电** → 连接家中 WiFi → 获取 IP
2. **Bridge 广播 IP** → UART2 发送 `BRIDGE_IP:192.168.x.x`
3. **ESP32-S3 接收** → 保存 IP 到 NVS
4. **用户打开配网页面** → 点击蓝牙 Tab
5. **API 转发** → `/bt/*` 请求通过 WiFi TCP 转发给 Bridge
6. **Bridge 执行** → 扫描/连接蓝牙音响
7. **音频播放** → I2S → A2DP → 蓝牙音响

### 4.5 配网页面 API

| 接口 | 方法 | 说明 |
|------|------|------|
| `/bt/status` | GET | 获取 BT 状态：连接/断开、已配对设备 |
| `/bt/scan` | GET | 开始扫描蓝牙设备 |
| `/bt/results` | GET | 获取扫描结果 |
| `/bt/connect` | POST | 连接指定设备 (`{"mac":"XX:XX:XX:XX:XX:XX"}`) |
| `/bt/disconnect` | POST | 断开当前连接 |
| `/bt/forget` | POST | 忘记已配对设备 (`{"mac":"..."}`) |
| `/bt/bridge_status` | GET | 查询桥接模式状态 |

### 4.6 NVS 存储

| Namespace | Key | 说明 |
|-----------|-----|------|
| `bt_bridge` | `ip` | Bridge 设备 IP 地址 |
| `bt_a2dp` | `mac` | 上次配对设备 MAC |
| `bt_a2dp` | `name` | 上次配对设备名称 |

### 4.7 关键代码文件

| 文件 | 说明 |
|------|------|
| `uart_bridge_client.cc/h` | ESP32-S3 侧 UART 监听 + HTTP 转发 |
| `bt_speaker.cc/h` | ESP32 Bridge 侧 A2DP Source 管理 |
| `i2s_slave_input.cc/h` | ESP32 Bridge 侧 I2S 从机接收 |
| `http_server.cc/h` | ESP32 Bridge 侧 HTTP API |
| `bt_bridge_main.cc` | ESP32 Bridge 主程序 |
| `wifi_configuration_ap.cc` | 配网页面 BT API 转发逻辑 |

---

## 5. WiFi 配网页面

### 5.1 功能描述

内置 Web，配网 + 蓝牙音响配置，通过手机/电脑浏览器访问。

### 5.2 访问方式

1. ElectronBot 作为 AP 热点：`Xiaozhi_XXXX`（无密码）
2. 或连接同一局域网后访问设备 IP

### 5.3 配网页面 Tab

| Tab | 功能 |
|-----|------|
| WiFi | 连接家中 WiFi |
| 蓝牙音响 | 扫描/连接蓝牙音箱（通过 ESP32 Bridge） |

### 5.4 已知限制

- ESP32-S3 硬件**不支持 Classic BT / A2DP**
- 蓝牙音响功能**必须配合 ESP32 Bridge 使用**
- 无 Bridge 时，配网页面会显示对应提示

---

## 6. 硬件接线图

详细接线图请参考：`docs/hardware_wiring_diagram.html`

---

## 7. 文件变更记录

### 7.1 新增文件

| 文件 | 说明 |
|------|------|
| `main/boards/electron-bot/seeed-sense-face/main/face_detector_main.cpp` | Seeed 端主程序（v2：持续发送位置追踪） |
| `main/boards/electron-bot/seeed-sense-face/main/color_gender_analyzer.c/h` | HSV 颜色 + 性别分析 |
| `main/boards/electron-bot/seeed-sense-face/main/uart_notifier.c/h` | UART 通知协议（v2：添加 face_track 事件） |
| `main/boards/electron-bot/face_detect_receiver.cc/h` | 主控 UART 解析（v2：支持追踪回调） |
| `main/boards/electron-bot/electron_bot_controller.cc` | 控制器（v2：添加人脸追踪任务） |
| `main/boards/electron-bot/uart_bridge_client.cc/h` | 蓝牙桥接客户端 |
| `docs/hardware_wiring_diagram.html` | 硬件接线图 |
| `docs/hardware_connection.html` | Seeed Sense 连接图 |

### 7.2 修改文件

| 文件 | 修改内容 |
|------|----------|
| `main/boards/electron-bot/electron_bot_controller.cc` | 增加个性化问候逻辑 |
| `main/boards/electron-bot/electron_bot.cc` | 注册 FaceDetectReceiver |
| `main/boards/electron-bot/config.h` | GPIO 引脚定义 |
| `managed_components/*/wifi_configuration_ap.cc` | BT API 转发、bridge_status 接口 |
| `managed_components/*/wifi_configuration.html` | 蓝牙 Tab UI |
| `main/boards/electron-bot/uart_bridge_client.cc` | UART RX GPIO 从 1 改为 2 |

### 7.3 ESP32 Bridge 项目

独立项目，位于：`/Users/jvine/Documents/xiaozhi-esp32-bridge/`

---

## 附录：版本信息

- **固件版本**：2.2.4
- **协议版本**：FaceDetect v2（支持 face_track 事件）
- **桥接固件**：ESP32 Bridge v1.0
- **人脸追踪功能**：v1.0（2026-05-02）
