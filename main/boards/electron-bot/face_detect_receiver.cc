/**
 * @file face_detect_receiver.cc
 * @brief 人员识别 UART 接收器实现（v2：颜色 + 性别 + 去重 + 人脸追踪）
 *
 * 运行在 ElectronBot 主控 (ESP32-S3) 上。
 *
 * 硬件连接（不变）：
 *   Seeed XIAO ESP32S3 Sense GPIO43 (TX) ──▶ ElectronBot GPIO1 (RX)
 *   Seeed XIAO ESP32S3 Sense GND         ──▶ ElectronBot GND
 *
 * 通信协议（v2）：
 *   波特率: 115200，数据格式: 8N1
 *   消息格式:
 *     打招呼事件: {"event":"person_greeted","color":"blue","gender":"male",
 *                  "count":1,"confidence":0.92,"person_id":3,
 *                  "face_x":100,"face_y":80,"face_w":80,"face_h":100}\n
 *     追踪事件:   {"event":"face_track","has_face":1,
 *                  "face_x":100,"face_y":80,"face_w":80,"face_h":100,
 *                  "center_x":140,"center_y":130}\n
 *
 * 去重说明：
 *   - 去重在 Seeed 端已完成（每个 person_id 冷却 30s）
 *   - ElectronBot 端仅做一层防抖保护（500ms 防连发）
 */

#include "face_detect_receiver.h"

#include <cstring>
#include <cstdlib>
#include <cctype>

#include "driver/uart.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TAG "FaceDetectReceiver"

/* ─── UART 配置（与 v1 完全相同，硬件不变） ─── */
#define FACE_UART_PORT    UART_NUM_2
#define FACE_UART_RX_PIN  GPIO_NUM_1    // 接 Seeed 板 TX
#define FACE_UART_TX_PIN  GPIO_NUM_NC   // 主控不需要向 Seeed 发送，设为 NC
#define FACE_UART_BAUD    115200
#define FACE_UART_BUF_SZ  512

/* ElectronBot 端本地防抖：500ms 内不重复触发 */
#define LOCAL_DEBOUNCE_MS 500

FaceDetectReceiver::FaceDetectReceiver() = default;

FaceDetectReceiver::~FaceDetectReceiver() {
    Stop();
}

void FaceDetectReceiver::Start(PersonGreetedCallback on_person_greeted,
                               FaceTrackCallback on_face_track) {
    callback_      = on_person_greeted;
    track_callback_ = on_face_track;
    running_      = true;

    uart_config_t uart_cfg = {
        .baud_rate  = FACE_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(FACE_UART_PORT,
                                        FACE_UART_BUF_SZ * 2, 0,
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(FACE_UART_PORT, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(FACE_UART_PORT,
                                 FACE_UART_TX_PIN,
                                 FACE_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART%d RX=GPIO%d @ %d bps — waiting for person events (v2)",
             FACE_UART_PORT, FACE_UART_RX_PIN, FACE_UART_BAUD);

    xTaskCreate(ReceiverTask, "face_rx", 4 * 1024, this, 4, &task_handle_);
}

void FaceDetectReceiver::Stop() {
    running_ = false;
    if (task_handle_ != nullptr) {
        vTaskDelete(task_handle_);
        task_handle_ = nullptr;
    }
    uart_driver_delete(FACE_UART_PORT);
}

// ─────────────────────────────────────────────────────────────────────────────
// 接收任务：逐字节读取，按 '\n' 分割 JSON 消息
// ─────────────────────────────────────────────────────────────────────────────
void FaceDetectReceiver::ReceiverTask(void* arg) {
    FaceDetectReceiver* self = static_cast<FaceDetectReceiver*>(arg);

    char    line_buf[384] = {};
    int     line_len      = 0;
    uint8_t byte;

    /* 本地防抖时间戳（毫秒，使用 xTaskGetTickCount） */
    TickType_t last_trigger_tick = 0;

    while (self->running_) {
        int ret = uart_read_bytes(FACE_UART_PORT, &byte, 1, pdMS_TO_TICKS(200));
        if (ret <= 0) {
            continue;
        }

        if (byte == '\n' || byte == '\r') {
            if (line_len > 0) {
                line_buf[line_len] = '\0';
                ESP_LOGD(TAG, "Received: %s", line_buf);

                /* 本地防抖 */
                TickType_t now = xTaskGetTickCount();
                if ((now - last_trigger_tick) * portTICK_PERIOD_MS < LOCAL_DEBOUNCE_MS) {
                    ESP_LOGD(TAG, "Local debounce, skip");
                    line_len = 0;
                    continue;
                }

                PersonInfo info;
                bool is_greet_event = false;
                if (self->ParseMessage(line_buf, line_len, info, is_greet_event)) {
                    // 如果是追踪事件，直接调用追踪回调
                    if (!is_greet_event && self->track_callback_) {
                        self->track_callback_(info.has_face, info.center_x, info.center_y);
                    }

                    // 如果是打招呼事件，调用打招呼回调
                    if (is_greet_event && self->callback_) {
                        std::string greeting = BuildGreetingText(info.color, info.gender);
                        ESP_LOGI(TAG, "Person event: pid=%d color=%s gender=%s → \"%s\"",
                                 info.person_id, info.color.c_str(),
                                 info.gender.c_str(), greeting.c_str());
                        self->callback_(info, greeting);
                        last_trigger_tick = now;
                    }
                }
                line_len = 0;
            }
        } else {
            if (line_len < (int)sizeof(line_buf) - 1) {
                line_buf[line_len++] = (char)byte;
            } else {
                line_len = 0;
                ESP_LOGW(TAG, "Line buffer overflow, discarding");
            }
        }
    }

    vTaskDelete(NULL);
}

// ─────────────────────────────────────────────────────────────────────────────
// 解析 v2 JSON 消息
//
// 期望格式:
//   打招呼事件: {"event":"person_greeted","color":"blue","gender":"male",
//                "count":1,"confidence":0.92,"person_id":3,
//                "face_x":100,"face_y":80,"face_w":80,"face_h":100}
//   追踪事件:   {"event":"face_track","has_face":1,
//                "face_x":100,"face_y":80,"face_w":80,"face_h":100,
//                "center_x":140,"center_y":130}
// 兼容旧格式: {"event":"face_detected","count":N,"confidence":X.XX}
//
// @param out_is_greet  [out] 是否是打招呼事件（true=person_greeted, false=face_track）
// ─────────────────────────────────────────────────────────────────────────────
bool FaceDetectReceiver::ParseMessage(const char* json, int /*len*/,
                                     PersonInfo& info, bool& out_is_greet) {
    cJSON* root = cJSON_Parse(json);
    if (!root) {
        ESP_LOGW(TAG, "JSON parse error: %s", json);
        return false;
    }

    bool ok = false;
    out_is_greet = false;

    // 初始化位置字段默认值
    info.has_face = false;
    info.face_x = info.face_y = -1;
    info.face_w = info.face_h = 0;
    info.center_x = info.center_y = -1;

    cJSON* event = cJSON_GetObjectItem(root, "event");
    if (!cJSON_IsString(event)) {
        cJSON_Delete(root);
        return false;
    }

    bool is_greet = (strcmp(event->valuestring, "person_greeted") == 0);
    bool is_track = (strcmp(event->valuestring, "face_track")     == 0);
    bool is_v1    = (strcmp(event->valuestring, "face_detected")  == 0);

    if (is_greet) {
        out_is_greet = true;
        cJSON* cnt   = cJSON_GetObjectItem(root, "count");
        cJSON* conf  = cJSON_GetObjectItem(root, "confidence");
        cJSON* color  = cJSON_GetObjectItem(root, "color");
        cJSON* gender = cJSON_GetObjectItem(root, "gender");
        cJSON* pid    = cJSON_GetObjectItem(root, "person_id");
        cJSON* fx     = cJSON_GetObjectItem(root, "face_x");
        cJSON* fy     = cJSON_GetObjectItem(root, "face_y");
        cJSON* fw     = cJSON_GetObjectItem(root, "face_w");
        cJSON* fh     = cJSON_GetObjectItem(root, "face_h");

        info.count      = cJSON_IsNumber(cnt)  ? cnt->valueint             : 1;
        info.confidence = cJSON_IsNumber(conf) ? (float)conf->valuedouble  : 0.9f;
        info.color      = (cJSON_IsString(color)  && color->valuestring)
                          ? color->valuestring  : "unknown";
        info.gender     = (cJSON_IsString(gender) && gender->valuestring)
                          ? gender->valuestring : "unknown";
        info.person_id  = cJSON_IsNumber(pid) ? pid->valueint : 0;
        info.has_face   = true;
        info.face_x     = cJSON_IsNumber(fx) ? fx->valueint : 0;
        info.face_y     = cJSON_IsNumber(fy) ? fy->valueint : 0;
        info.face_w     = cJSON_IsNumber(fw) ? fw->valueint : 0;
        info.face_h     = cJSON_IsNumber(fh) ? fh->valueint : 0;
        info.center_x   = info.face_x + info.face_w / 2;
        info.center_y   = info.face_y + info.face_h / 2;

        ok = true;
    } else if (is_track) {
        out_is_greet = false;
        cJSON* hf    = cJSON_GetObjectItem(root, "has_face");
        cJSON* cx    = cJSON_GetObjectItem(root, "center_x");
        cJSON* cy    = cJSON_GetObjectItem(root, "center_y");
        cJSON* fx    = cJSON_GetObjectItem(root, "face_x");
        cJSON* fy    = cJSON_GetObjectItem(root, "face_y");
        cJSON* fw    = cJSON_GetObjectItem(root, "face_w");
        cJSON* fh    = cJSON_GetObjectItem(root, "face_h");

        info.has_face = cJSON_IsNumber(hf) ? (hf->valueint != 0) : false;
        info.center_x  = cJSON_IsNumber(cx) ? cx->valueint : -1;
        info.center_y  = cJSON_IsNumber(cy) ? cy->valueint : -1;
        info.face_x    = cJSON_IsNumber(fx) ? fx->valueint : -1;
        info.face_y    = cJSON_IsNumber(fy) ? fy->valueint : -1;
        info.face_w    = cJSON_IsNumber(fw) ? fw->valueint : 0;
        info.face_h    = cJSON_IsNumber(fh) ? fh->valueint : 0;

        ok = true;
    } else if (is_v1) {
        out_is_greet = true;
        cJSON* cnt   = cJSON_GetObjectItem(root, "count");
        cJSON* conf  = cJSON_GetObjectItem(root, "confidence");

        info.count      = cJSON_IsNumber(cnt)  ? cnt->valueint             : 1;
        info.confidence = cJSON_IsNumber(conf) ? (float)conf->valuedouble  : 0.9f;
        info.color      = "unknown";
        info.gender     = "unknown";
        info.person_id  = 0;
        info.has_face   = true;

        ok = true;
    }

    cJSON_Delete(root);
    return ok;
}

// ─────────────────────────────────────────────────────────────────────────────
// 颜色/性别 → 中文
// ─────────────────────────────────────────────────────────────────────────────
const char* FaceDetectReceiver::ColorToChinese(const std::string& color_en) {
    if (color_en == "red")    return "红色";
    if (color_en == "orange") return "橙色";
    if (color_en == "yellow") return "黄色";
    if (color_en == "green")  return "绿色";
    if (color_en == "cyan")   return "青色";
    if (color_en == "blue")   return "蓝色";
    if (color_en == "purple") return "紫色";
    if (color_en == "white")  return "白色";
    if (color_en == "black")  return "黑色";
    if (color_en == "gray")   return "灰色";
    return nullptr;  // unknown → 不加颜色前缀
}

const char* FaceDetectReceiver::GenderToChinese(const std::string& gender_en) {
    if (gender_en == "male")   return "小哥";
    if (gender_en == "female") return "美女";
    return "朋友";
}

// ─────────────────────────────────────────────────────────────────────────────
// 生成中文问候语
// 例如：
//   color=blue,  gender=male   → "你好蓝色的小哥"
//   color=red,   gender=female → "你好红色的美女"
//   color=unknown,gender=male  → "你好小哥"
//   color=unknown,gender=unknown → "你好朋友"
// ─────────────────────────────────────────────────────────────────────────────
std::string FaceDetectReceiver::BuildGreetingText(const std::string& color,
                                                   const std::string& gender) {
    const char* color_cn  = ColorToChinese(color);
    const char* gender_cn = GenderToChinese(gender);

    std::string text = "你好";
    if (color_cn != nullptr) {
        text += color_cn;
        text += "的";
    }
    text += gender_cn;

    return text;
}
