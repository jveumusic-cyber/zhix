#pragma once

/**
 * @file face_detect_receiver.h
 * @brief 人员识别 UART 接收器（v2：颜色 + 性别 + 去重）
 *
 * ElectronBot 主控上运行的 UART 监听模块。
 * 持续监听来自 Seeed XIAO ESP32S3 Sense 的 JSON 通知，
 * 根据颜色和性别生成个性化问候语，每个 person_id 只打一次招呼。
 *
 * 新 JSON 协议（v2）：
 *   {"event":"person_greeted","color":"blue","gender":"male",
 *    "count":1,"confidence":0.92,"person_id":3}\n
 */

#include <functional>
#include <string>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/** 人员识别结果 */
struct PersonInfo {
    int         count;       /**< 画面中检测到的人脸数 */
    float       confidence;  /**< 最高置信度 */
    std::string color;       /**< 衣服颜色英文名 */
    std::string gender;      /**< 性别英文名 */
    int         person_id;   /**< 人脸位置区域 ID（0~7），用于去重 */

    // 人脸位置信息（用于追踪）
    bool        has_face;    /**< 是否检测到人脸 */
    int         face_x;      /**< 人脸框左上角 X */
    int         face_y;      /**< 人脸框左上角 Y */
    int         face_w;      /**< 人脸框宽度 */
    int         face_h;      /**< 人脸框高度 */
    int         center_x;    /**< 人脸中心 X */
    int         center_y;    /**< 人脸中心 Y */
};

/**
 * @brief 人员识别接收器类（v2）
 */
class FaceDetectReceiver {
public:
/**
 * @brief 人员识别事件回调类型
 *
 * @param info   识别结果（颜色、性别、置信度等）
 * @param greeting_text 已拼好的中文问候语（如 "你好蓝色的小哥"）
 */
using PersonGreetedCallback = std::function<void(const PersonInfo& info,
                                                 const std::string& greeting_text)>;

/**
 * @brief 人脸位置追踪回调类型
 *
 * @param has_face  是否检测到人脸
 * @param center_x 人脸中心 X（图像坐标系，0~319）
 * @param center_y 人脸中心 Y（图像坐标系，0~239）
 */
using FaceTrackCallback = std::function<void(bool has_face, int center_x, int center_y)>;

    FaceDetectReceiver();
    ~FaceDetectReceiver();

    /**
     * @brief 初始化并启动 UART 接收任务
     *
     * @param on_person_greeted 识别到新人员时的回调
     * @param on_face_track     人脸位置追踪回调（可选，传 nullptr 则不启用追踪）
     */
    void Start(PersonGreetedCallback on_person_greeted,
               FaceTrackCallback on_face_track = nullptr);

    /**
     * @brief 停止接收任务
     */
    void Stop();

private:
    PersonGreetedCallback callback_;
    FaceTrackCallback     track_callback_;
    TaskHandle_t          task_handle_ = nullptr;
    bool                  running_     = false;

    static void ReceiverTask(void* arg);

    /**
     * @brief 解析 v2 JSON 消息
     * @param out_is_greet [out] 是否是打招呼事件
     * @return true 解析成功
     */
    bool ParseMessage(const char* json, int len, PersonInfo& out_info, bool& out_is_greet);

    /**
     * @brief 根据颜色英文名和性别英文名生成中文问候语
     */
    static std::string BuildGreetingText(const std::string& color,
                                         const std::string& gender);

    /**
     * @brief 颜色英文名 → 中文
     */
    static const char* ColorToChinese(const std::string& color_en);

    /**
     * @brief 性别英文名 → 中文称谓
     */
    static const char* GenderToChinese(const std::string& gender_en);
};
