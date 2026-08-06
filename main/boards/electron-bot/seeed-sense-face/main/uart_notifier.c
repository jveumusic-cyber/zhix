/**
 * uart_notifier.c  —  v2 版本
 *
 * UART 通知发送模块实现
 * 运行在 Seeed Studio XIAO ESP32S3 Sense 上
 *
 * 检测到人员时，通过 UART1 (GPIO43 TX) 向 ElectronBot 主控发送 JSON 通知。
 */

#include "uart_notifier.h"

#include <stdio.h>
#include <string.h>
#include "esp_log.h"
#include "driver/uart.h"

#define TAG "UartNotifier"

#define UART_BUF_SIZE 512

void uart_notifier_init(void) {
    uart_config_t uart_config = {
        .baud_rate  = FACE_NOTIFY_UART_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    ESP_ERROR_CHECK(uart_driver_install(FACE_NOTIFY_UART_PORT,
                                        UART_BUF_SIZE * 2,   /* RX buf */
                                        UART_BUF_SIZE * 2,   /* TX buf */
                                        0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(FACE_NOTIFY_UART_PORT, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(FACE_NOTIFY_UART_PORT,
                                 FACE_NOTIFY_UART_TX_PIN,
                                 FACE_NOTIFY_UART_RX_PIN,
                                 UART_PIN_NO_CHANGE,
                                 UART_PIN_NO_CHANGE));

    ESP_LOGI(TAG, "UART notifier v2 initialized on UART%d TX=GPIO%d @ %d bps",
             FACE_NOTIFY_UART_PORT, FACE_NOTIFY_UART_TX_PIN, FACE_NOTIFY_UART_BAUD);
}

void uart_notifier_send_person_greeted(int count, float confidence,
                                       const char *color_name,
                                       const char *gender_name,
                                       int person_id,
                                       int face_x, int face_y,
                                       int face_w, int face_h)
{
    char msg[256];
    int len = snprintf(msg, sizeof(msg),
        "{\"event\":\"person_greeted\","
        "\"color\":\"%s\","
        "\"gender\":\"%s\","
        "\"count\":%d,"
        "\"confidence\":%.2f,"
        "\"person_id\":%d,"
        "\"face_x\":%d,"
        "\"face_y\":%d,"
        "\"face_w\":%d,"
        "\"face_h\":%d}\n",
        color_name ? color_name : "unknown",
        gender_name ? gender_name : "unknown",
        count,
        (double)confidence,
        person_id,
        face_x, face_y, face_w, face_h);

    uart_write_bytes(FACE_NOTIFY_UART_PORT, msg, len);
    ESP_LOGD(TAG, "Sent greet: %.*s", len - 1, msg);
}

void uart_notifier_send_face_track(int has_face,
                                   int face_x, int face_y,
                                   int face_w, int face_h,
                                   int center_x, int center_y)
{
    char msg[128];
    int len = snprintf(msg, sizeof(msg),
        "{\"event\":\"face_track\","
        "\"has_face\":%d,"
        "\"face_x\":%d,"
        "\"face_y\":%d,"
        "\"face_w\":%d,"
        "\"face_h\":%d,"
        "\"center_x\":%d,"
        "\"center_y\":%d}\n",
        has_face,
        face_x, face_y, face_w, face_h,
        center_x, center_y);

    uart_write_bytes(FACE_NOTIFY_UART_PORT, msg, len);
    ESP_LOGV(TAG, "Track: %.*s", len - 1, msg);
}
