/**
 * face_detector_main.cpp  —  v2：颜色 + 性别识别 + 去重打招呼
 *
 * Seeed Studio XIAO ESP32S3 Sense — 人员检测主程序
 *
 * 功能：
 *   - 使用板载 OV3660 摄像头持续抓帧
 *   - 使用 ESP-DL HumanFaceDetect 进行人脸检测
 *   - 对人脸框下方区域做 HSV 颜色统计，识别衣服主色
 *   - 通过人脸宽高比粗判性别
 *   - 去重逻辑：同一位置（person_id）在 GREET_COOLDOWN_MS 内只触发一次；
 *     超过 PRESENCE_TIMEOUT_MS 没有任何人出现时重置所有记录
 *   - 通过 UART1 向 ElectronBot 主控发送 JSON 通知
 *
 * 硬件：Seeed Studio XIAO ESP32S3 Sense (OV3660 摄像头)
 * 目标芯片：ESP32-S3
 * IDF 版本：>= 5.1
 */

#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_camera.h"
#include "esp_timer.h"

/* 人脸检测 */
#include "human_face_detect.hpp"

/* JPEG 解码 */
#include "img_converters.h"

/* UART 通知（v2） */
#include "uart_notifier.h"

/* 颜色 & 性别分析 */
#include "color_gender_analyzer.h"

#define TAG "FaceDetector"

/* ─── 摄像头引脚（Seeed XIAO ESP32S3 Sense OV3660） ─── */
#define CAM_PIN_PWDN    (-1)
#define CAM_PIN_RESET   (-1)
#define CAM_PIN_XCLK    10
#define CAM_PIN_SIOD    40
#define CAM_PIN_SIOC    39
#define CAM_PIN_D7      48
#define CAM_PIN_D6      11
#define CAM_PIN_D5      12
#define CAM_PIN_D4      14
#define CAM_PIN_D3      16
#define CAM_PIN_D2      18
#define CAM_PIN_D1      17
#define CAM_PIN_D0      15
#define CAM_PIN_VSYNC   38
#define CAM_PIN_HREF    47
#define CAM_PIN_PCLK    13

/* 图像尺寸（QVGA 320×240） */
#define IMG_W 320
#define IMG_H 240

/*
 * GREET_COOLDOWN_MS：同一个 person_id 两次打招呼之间的最短间隔
 * 设置为 30 秒，防止同一人在视野里重复触发
 */
#define GREET_COOLDOWN_MS   30000

/*
 * PRESENCE_TIMEOUT_MS：连续多少毫秒没有检测到任何人，
 * 就重置所有 person_id 记录，允许下次重新打招呼。
 * 设置为 12 秒
 */
#define PRESENCE_TIMEOUT_MS 12000

/*
 * PERSON_ID_SLOTS：支持同时跟踪的不同人脸位置数量
 * person_id = face_center_x / (IMG_W / PERSON_ID_SLOTS)，范围 [0, PERSON_ID_SLOTS-1]
 */
#define PERSON_ID_SLOTS 8

/* ─── 去重状态 ─── */
static int64_t s_last_greeted_us[PERSON_ID_SLOTS];   /* 每个 slot 上次打招呼时间 */
static int64_t s_last_seen_any_us = 0;               /* 上次检测到任何人的时间 */

/* 颜色枚举 → 英文名（用于 JSON） */
static const char *color_to_english(cloth_color_t c)
{
    switch (c) {
        case COLOR_RED:     return "red";
        case COLOR_ORANGE:  return "orange";
        case COLOR_YELLOW:  return "yellow";
        case COLOR_GREEN:   return "green";
        case COLOR_CYAN:    return "cyan";
        case COLOR_BLUE:    return "blue";
        case COLOR_PURPLE:  return "purple";
        case COLOR_WHITE:   return "white";
        case COLOR_BLACK:   return "black";
        case COLOR_GRAY:    return "gray";
        default:            return "unknown";
    }
}

/* 追踪节流：限制追踪消息发送频率，避免 UART 阻塞 */
static int64_t s_last_track_us = 0;
#define TRACK_INTERVAL_MS 100  /* 每 100ms 发一次追踪消息（约10fps） */

/* 性别枚举 → 英文名 */
static const char *gender_to_english(gender_t g)
{
    switch (g) {
        case GENDER_MALE:   return "male";
        case GENDER_FEMALE: return "female";
        default:            return "unknown";
    }
}

/* ─── 摄像头初始化 ─── */
static esp_err_t camera_init(void)
{
    camera_config_t config = {};
    config.ledc_channel  = LEDC_CHANNEL_0;
    config.ledc_timer    = LEDC_TIMER_0;
    config.pin_d0        = CAM_PIN_D0;
    config.pin_d1        = CAM_PIN_D1;
    config.pin_d2        = CAM_PIN_D2;
    config.pin_d3        = CAM_PIN_D3;
    config.pin_d4        = CAM_PIN_D4;
    config.pin_d5        = CAM_PIN_D5;
    config.pin_d6        = CAM_PIN_D6;
    config.pin_d7        = CAM_PIN_D7;
    config.pin_xclk      = CAM_PIN_XCLK;
    config.pin_pclk      = CAM_PIN_PCLK;
    config.pin_vsync     = CAM_PIN_VSYNC;
    config.pin_href      = CAM_PIN_HREF;
    config.pin_sccb_sda  = CAM_PIN_SIOD;
    config.pin_sccb_scl  = CAM_PIN_SIOC;
    config.pin_pwdn      = CAM_PIN_PWDN;
    config.pin_reset     = CAM_PIN_RESET;
    config.xclk_freq_hz  = 15000000;
    config.pixel_format  = PIXFORMAT_JPEG;
    config.frame_size    = FRAMESIZE_QVGA;
    config.jpeg_quality  = 10;
    config.fb_count      = 2;
    config.fb_location   = CAMERA_FB_IN_PSRAM;
    config.grab_mode     = CAMERA_GRAB_WHEN_EMPTY;

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera init failed: 0x%x", err);
        return err;
    }

    sensor_t *s = esp_camera_sensor_get();
    s->set_vflip(s, 1);
    s->set_hmirror(s, 0);
    s->set_brightness(s, 1);
    s->set_saturation(s, -1);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_gain_ctrl(s, 1);

    ESP_LOGI(TAG, "Camera initialized: OV3660 QVGA JPEG");
    return ESP_OK;
}

/* ─── 人脸检测任务 ─── */
static void face_detect_task(void *arg)
{
    HumanFaceDetect detector;
    ESP_LOGI(TAG, "Face detection task v2 started (color + gender + dedup)");

    /* 初始化去重状态 */
    memset(s_last_greeted_us, 0, sizeof(s_last_greeted_us));

    while (true) {
        int64_t now_us = esp_timer_get_time();

        /* ─── 超时重置：若长时间无人，清空所有打招呼记录 ─── */
        if (s_last_seen_any_us > 0) {
            int64_t absent_ms = (now_us - s_last_seen_any_us) / 1000;
            if (absent_ms >= PRESENCE_TIMEOUT_MS) {
                ESP_LOGI(TAG, "No person for %lldms, resetting greet records", absent_ms);
                memset(s_last_greeted_us, 0, sizeof(s_last_greeted_us));
                s_last_seen_any_us = 0;
            }
        }

        /* 抓帧 */
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(TAG, "Failed to get frame buffer");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* JPEG → RGB888 解码 */
        const size_t rgb_size = IMG_W * IMG_H * 3;
        uint8_t *rgb_buf = (uint8_t *)malloc(rgb_size);
        if (!rgb_buf) {
            ESP_LOGE(TAG, "Failed to alloc RGB buffer");
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        bool ok = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_JPEG, rgb_buf);
        esp_camera_fb_return(fb);   /* 尽早归还帧缓冲 */

        if (!ok) {
            ESP_LOGW(TAG, "JPEG -> RGB888 conversion failed");
            free(rgb_buf);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        /* 构建 esp-dl 图像结构 */
        dl::image::img_t img;
        img.data     = rgb_buf;
        img.width    = IMG_W;
        img.height   = IMG_H;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB888;

        /* 运行人脸检测 */
        auto &results = detector.run(img);

        if (!results.empty()) {
            /* 更新"有人在场"时间戳 */
            s_last_seen_any_us = now_us;

            int face_count = (int)results.size();
            float best_score = 0.0f;
            for (auto &r : results) {
                if (r.score > best_score) best_score = r.score;
            }

            /*
             * 遍历每个检测到的人脸，决定是否打招呼
             * 每张人脸独立判断 person_id 冷却
             */
            for (auto &r : results) {
                if (r.box.size() < 4) continue;

                int x1 = r.box[0], y1 = r.box[1];
                int x2 = r.box[2], y2 = r.box[3];
                int face_w = x2 - x1;
                int face_h = y2 - y1;

                /* 计算 person_id（按人脸中心 X 分区，0~PERSON_ID_SLOTS-1） */
                int center_x = (x1 + x2) / 2;
                int center_y = (y1 + y2) / 2;
                int pid = center_x * PERSON_ID_SLOTS / IMG_W;
                if (pid < 0)                pid = 0;
                if (pid >= PERSON_ID_SLOTS) pid = PERSON_ID_SLOTS - 1;

                /* 检查冷却期 */
                int64_t elapsed_ms = (s_last_greeted_us[pid] == 0)
                                     ? (int64_t)GREET_COOLDOWN_MS   /* 从未打过，可以触发 */
                                     : (now_us - s_last_greeted_us[pid]) / 1000;

                if (elapsed_ms < GREET_COOLDOWN_MS) {
                    ESP_LOGD(TAG, "person_id=%d cooldown=%lldms, skip", pid, elapsed_ms);
                    continue;
                }

                /* ─── 颜色分析 ─── */
                cloth_color_t color = analyze_cloth_color(rgb_buf,
                                                          IMG_W, IMG_H,
                                                          x1, y1, x2, y2);

                /* ─── 性别粗判 ─── */
                gender_t gender = estimate_gender(x1, y1, x2, y2);

                const char *color_en  = color_to_english(color);
                const char *gender_en = gender_to_english(gender);

                ESP_LOGI(TAG,
                    "Greet! pid=%d face=(%d,%d)-(%d,%d) color=%s(%s) gender=%s score=%.2f",
                    pid, x1, y1, x2, y2,
                    color_to_chinese(color), color_en,
                    gender_en, r.score);

                /* 发送 UART 通知（带坐标） */
                uart_notifier_send_person_greeted(face_count, best_score,
                                                 color_en, gender_en, pid,
                                                 x1, y1, face_w, face_h);

                /* 更新该 person_id 的打招呼时间 */
                s_last_greeted_us[pid] = now_us;

                /*
                 * 每帧最多打一次招呼（避免多人同时触发时语音叠加）
                 * 如果需要同帧多人打招呼，删除此 break 即可。
                 */
                break;
            }

            /* ─── 发送追踪消息：使用第一个人脸的坐标 ─── */
            int64_t track_elapsed_ms = (now_us - s_last_track_us) / 1000;
            if (track_elapsed_ms >= TRACK_INTERVAL_MS) {
                auto &r = *results.begin();  /* 跟踪最大的/置信度最高的人脸 */
                int x1 = r.box[0], y1 = r.box[1];
                int x2 = r.box[2], y2 = r.box[3];
                int face_w = x2 - x1;
                int face_h = y2 - y1;
                int center_x = (x1 + x2) / 2;
                int center_y = (y1 + y2) / 2;

                uart_notifier_send_face_track(1, x1, y1, face_w, face_h,
                                             center_x, center_y);
                s_last_track_us = now_us;
            }
        } else {
            /* 没人脸时也发送追踪消息（让主控知道无人） */
            int64_t track_elapsed_ms = (now_us - s_last_track_us) / 1000;
            if (track_elapsed_ms >= TRACK_INTERVAL_MS) {
                uart_notifier_send_face_track(0, -1, -1, 0, 0, -1, -1);
                s_last_track_us = now_us;
            }
        }

        free(rgb_buf);

        /* ~10fps 检测频率 */
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

/* ─── 主入口 ─── */
extern "C" void app_main(void)
{
    ESP_LOGI(TAG, "=== Seeed XIAO ESP32S3 Sense - Person Greeter v2 ===");
    ESP_LOGI(TAG, "Features: color detection + gender estimation + dedup greet");

    /* 初始化 UART 通知模块 */
    uart_notifier_init();

    /* 初始化摄像头 */
    if (camera_init() != ESP_OK) {
        ESP_LOGE(TAG, "Fatal: camera init failed, halting");
        while (1) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    /* 创建人脸检测任务（固定到 APP_CPU 核心 1） */
    xTaskCreatePinnedToCore(
        face_detect_task,
        "face_detect",
        16 * 1024,  /* 16KB 栈，HumanFaceDetect 需要较大栈 */
        NULL,
        5,
        NULL,
        1           /* APP_CPU core 1 */
    );

    ESP_LOGI(TAG, "Person greeter task created, monitoring started");
}
