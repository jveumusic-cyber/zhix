#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief UART 通知发送模块
 *
 * 通过 UART 向 ElectronBot 主控发送人员识别事件 JSON 消息。
 * 波特率: 115200，数据位: 8，停止位: 1，无校验
 *
 * 协议（v2）消息格式：
 *
 *   打招呼事件（只在满足条件时发送一次）：
 *   {"event":"person_greeted","color":"blue","gender":"male",
 *    "count":1,"confidence":0.92,"person_id":42,"face_x":160,"face_y":120,
 *    "face_w":80,"face_h":100}\n
 *
 *   位置追踪事件（每帧持续发送，有人脸时发，没人也发）：
 *   {"event":"face_track","has_face":1,"face_x":160,"face_y":120,
 *    "face_w":80,"face_h":100,"center_x":160,"center_y":120}\n
 *
 * 字段说明：
 *   event       事件类型："person_greeted" | "face_track"
 *   color       衣服主色（red/orange/yellow/green/cyan/blue/purple/white/black/gray/unknown）
 *   gender      粗判性别（male/female/unknown）
 *   count       画面中检测到的人脸数量
 *   confidence  最高置信度 [0.0, 1.0]
 *   person_id   人脸中心 X 区域编号（0~7），用于去重
 *   has_face    是否有检测到人脸（0/1）
 *   face_x/y    人脸框左上角坐标
 *   face_w/h    人脸框宽高
 *   center_x/y  人脸中心坐标
 */

#include "driver/uart.h"
#include "driver/gpio.h"

/* UART 配置 — Seeed XIAO ESP32S3 Sense TX 引脚 */
#define FACE_NOTIFY_UART_PORT   UART_NUM_1
#define FACE_NOTIFY_UART_TX_PIN GPIO_NUM_43   /* Seeed XIAO ESP32S3 Sense 的 D6/TX1 引脚 */
#define FACE_NOTIFY_UART_RX_PIN GPIO_NUM_44   /* 不使用，但需配置 */
#define FACE_NOTIFY_UART_BAUD   115200

/**
 * @brief 初始化 UART 通知模块
 */
void uart_notifier_init(void);

/**
 * @brief 发送人员检测打招呼通知（v2 协议）
 *
 * @param count       检测到的人脸数量
 * @param confidence  最高置信度 (0.0 ~ 1.0)
 * @param color_name  衣服颜色英文名（如 "blue"、"red"）
 * @param gender_name 性别英文名（"male" / "female" / "unknown"）
 * @param person_id   人脸位置区域 ID（用于去重判断）
 * @param face_x      人脸框左上角 X
 * @param face_y      人脸框左上角 Y
 * @param face_w      人脸框宽度
 * @param face_h      人脸框高度
 */
void uart_notifier_send_person_greeted(int count, float confidence,
                                       const char *color_name,
                                       const char *gender_name,
                                       int person_id,
                                       int face_x, int face_y,
                                       int face_w, int face_h);

/**
 * @brief 发送人脸位置追踪事件（每帧持续发送）
 *
 * 用于 ElectronBot 主控追踪人脸位置。
 *
 * @param has_face   是否检测到人脸（0=无人, 1=有人）
 * @param face_x     人脸框左上角 X（无人时为-1）
 * @param face_y     人脸框左上角 Y（无人时为-1）
 * @param face_w     人脸框宽度（无人时为0）
 * @param face_h     人脸框高度（无人时为0）
 * @param center_x   人脸中心 X（无人时为-1）
 * @param center_y   人脸中心 Y（无人时为-1）
 */
void uart_notifier_send_face_track(int has_face,
                                   int face_x, int face_y,
                                   int face_w, int face_h,
                                   int center_x, int center_y);

#ifdef __cplusplus
}
#endif
