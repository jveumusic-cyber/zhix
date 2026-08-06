# Seeed Studio XIAO ESP32S3 Sense — 人脸检测从机固件

## 简介

本目录包含运行在 **Seeed Studio XIAO ESP32S3 Sense** 上的独立人脸检测固件。

该固件持续使用板载 OV2640 摄像头检测人脸，检测到人脸时通过 **UART** 向 ElectronBot 主控发送通知。

---

## 工作原理

```
[Seeed XIAO ESP32S3 Sense]          [ElectronBot 主控 ESP32-S3]
        OV2640 摄像头
             ↓ 每帧检测
        人脸检测算法
     (esp_face_detector)
             ↓ 检测到人脸
     发送 UART 消息: {"event":"face_detected"}\n
             ──────────── UART TX/RX ──────────▶
                                         接收通知
                                         QueueAction: 举右手
                                         播放TTS: "你好很高兴见到你"
                                         QueueAction: 放右手
```

---

## 硬件连接

| Seeed XIAO ESP32S3 Sense | ElectronBot ESP32-S3 | 说明 |
|--------------------------|----------------------|------|
| GPIO43 (TX/D6)           | GPIO1 (RX)           | 串口数据线 |
| GND                      | GND                  | 共地 |
| 3.3V                     | 3.3V                 | 电源（可选，若独立供电则无需） |

> ⚠️ 两块板子必须共地！

---

## 固件文件

- `face_detector_main.cpp` — 主程序入口
- `face_detector.h/.cpp`  — 人脸检测核心逻辑（基于 esp_face_detector 或 SSCMA）
- `uart_notifier.h/.cpp`  — UART 通知发送模块
- `CMakeLists.txt`         — 独立工程构建配置（需单独烧录到 Seeed 板）

---

## 编译与烧录

此固件是独立的 ESP-IDF 工程，需要单独编译烧录到 Seeed XIAO ESP32S3 Sense 板。

```bash
cd main/boards/electron-bot/seeed-sense-face
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyUSB0 flash
```

---

## 通信协议

Seeed 板通过 UART 以 115200 bps 发送 JSON 行：

```json
{"event":"face_detected","confidence":0.92,"count":1}
```

ElectronBot 主控收到后立即触发打招呼动作序列。

---

## 防抖说明

为避免同一张人脸反复触发，固件内置 **5秒冷却计时器**：检测到人脸后，5秒内不再重复发送通知。
