/*
    Electron Bot机器人控制器 - MCP协议版本
    新增：人员识别自动问候功能（v2：颜色 + 性别 + 去重 + 人脸追踪）

    功能说明：
      Seeed XIAO ESP32S3 Sense 识别到新出现的人员时，通过 UART 通知主控。
      主控根据识别到的衣服颜色和粗判性别，生成个性化问候语，
      例如："你好蓝色的小哥"、"你好红色的美女"。
      同一位置的人员在 30 秒冷却期内只打一次招呼。
      超过 12 秒没有人出现时重置记录，允许重新打招呼。

      同时支持人脸追踪功能：持续跟踪人脸位置，
      头部跟随人脸左右/上下，身体跟随大范围左右移动。

    语音说明：
      调用 WakeWordInvoke(greeting_text)，将个性化问候语发送给小智服务器，
      小智 TTS 自动朗读问候语，无需录制 greeting.ogg。
*/

#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

#include <cstring>
#include <string_view>

#include "application.h"
#include "board.h"
#include "config.h"
#include "display/display.h"
#include "face_detect_receiver.h"
#include "mcp_server.h"
#include "movements.h"
#include "sdkconfig.h"
#include "settings.h"

#define TAG "ElectronBotController"

struct ElectronBotActionParams {
    int action_type;
    int steps;
    int speed;
    int direction;
    int amount;
};

class ElectronBotController {
private:
    Otto electron_bot_;
    TaskHandle_t action_task_handle_ = nullptr;
    QueueHandle_t action_queue_;
    bool is_action_in_progress_ = false;

    // 人脸检测接收器
    FaceDetectReceiver face_detect_receiver_;

    // 待播报的个性化问候语（由 UART 接收回调设置，ActionTask 读取）
    // 仅在 QueueAction(ACTION_FACE_GREET) 入队前写入，ActionTask 执行时读取，
    // 两者在 FreeRTOS 任务调度下不会并发访问，无需互斥量。
    std::string pending_greeting_ = "你好朋友";

    // ─── 人脸追踪相关 ───────────────────────────────────────────────────────────
    // 追踪状态
    bool tracking_enabled_ = true;          // 是否启用追踪
    bool face_last_seen_ = false;         // 上次是否有人脸
    int last_face_x_ = 160;               // 上次人脸中心 X（图像坐标，0~319）
    int last_face_y_ = 120;               // 上次人脸中心 Y（图像坐标，0~239）

    // 舵机当前位置（累计偏移量）
    int head_offset_x_ = 0;               // 头部左右偏移（-30 ~ +30 度）
    int head_offset_y_ = 0;               // 头部上下偏移（-15 ~ +15 度）
    int body_offset_x_ = 0;               // 身体左右偏移（-90 ~ +90 度）

    // 追踪阈值
    static constexpr int TRACK_DEAD_ZONE = 15;   // 死区：中心±15像素内不动作
    static constexpr int HEAD_STEP = 2;           // 头部每次移动步进
    static constexpr int BODY_STEP = 5;           // 身体每次移动步进
    static constexpr int HEAD_MAX_X = 30;         // 头部最大左右偏移
    static constexpr int HEAD_MAX_Y = 15;         // 头部最大上下偏移
    static constexpr int BODY_MAX = 45;          // 身体最大左右偏移
    static constexpr int TRACK_TIMEOUT_MS = 3000; // 无人脸超时时间（毫秒）
    int64_t last_face_time_ms_ = 0;              // 上次看到人脸的时间

    // 追踪任务
    TaskHandle_t tracking_task_handle_ = nullptr;

    // 人脸追踪任务：定期检查人脸位置并驱动舵机（内联定义）

    enum ActionType {
        // 手部动作 1-12
        ACTION_HAND_LEFT_UP = 1,      // 举左手
        ACTION_HAND_RIGHT_UP = 2,     // 举右手
        ACTION_HAND_BOTH_UP = 3,      // 举双手
        ACTION_HAND_LEFT_DOWN = 4,    // 放左手
        ACTION_HAND_RIGHT_DOWN = 5,   // 放右手
        ACTION_HAND_BOTH_DOWN = 6,    // 放双手
        ACTION_HAND_LEFT_WAVE = 7,    // 挥左手
        ACTION_HAND_RIGHT_WAVE = 8,   // 挥右手
        ACTION_HAND_BOTH_WAVE = 9,    // 挥双手
        ACTION_HAND_LEFT_FLAP = 10,   // 拍打左手
        ACTION_HAND_RIGHT_FLAP = 11,  // 拍打右手
        ACTION_HAND_BOTH_FLAP = 12,   // 拍打双手

        // 身体动作 13-14
        ACTION_BODY_TURN_LEFT = 13,    // 左转
        ACTION_BODY_TURN_RIGHT = 14,   // 右转
        ACTION_BODY_TURN_CENTER = 15,  // 回中心

        // 头部动作 16-20
        ACTION_HEAD_UP = 16,          // 抬头
        ACTION_HEAD_DOWN = 17,        // 低头
        ACTION_HEAD_NOD_ONCE = 18,    // 点头一次
        ACTION_HEAD_CENTER = 19,      // 回中心
        ACTION_HEAD_NOD_REPEAT = 20,  // 连续点头

        // 系统动作 21
        ACTION_HOME = 21,  // 复位到初始位置

        // 人脸检测问候动作 22 (内部使用，不通过 MCP 暴露)
        ACTION_FACE_GREET = 22,  // 举右手 → 说"你好很高兴见到你" → 放右手

        // 人脸追踪动作 23-24 (内部使用)
        ACTION_FACE_TRACK_HEAD_LEFT = 23,   // 头部左转
        ACTION_FACE_TRACK_HEAD_RIGHT = 24,   // 头部右转
        ACTION_FACE_TRACK_HEAD_UP = 25,       // 头部上抬
        ACTION_FACE_TRACK_HEAD_DOWN = 26,     // 头部下低
        ACTION_FACE_TRACK_BODY_LEFT = 27,     // 身体左转
        ACTION_FACE_TRACK_BODY_RIGHT = 28,    // 身体右转
        ACTION_FACE_TRACK_HOME = 29           // 回初始位置
    };

    // ─── 人脸追踪任务实现 ─────────────────────────────────────────────────────
    static void TrackingTask(void* arg) {
        ElectronBotController* self = static_cast<ElectronBotController*>(arg);
        self->RunTracking();
        vTaskDelete(NULL);
    }

    void RunTracking() {
        ESP_LOGI(TAG, "Face tracking task started (smooth mode)");

        while (true) {
            if (!tracking_enabled_) {
                vTaskDelay(pdMS_TO_TICKS(500));
                continue;
            }

            int64_t now_ms = esp_timer_get_time() / 1000;

            if (!face_last_seen_) {
                // 无人脸超过超时时间，平滑回正
                if ((now_ms - last_face_time_ms_) > TRACK_TIMEOUT_MS) {
                    if (head_offset_y_ != 0 || body_offset_x_ != 0) {
                        // 每次向中心移动 2 度
                        const int RETURN_STEP = 2;
                        if (body_offset_x_ > RETURN_STEP) {
                            body_offset_x_ -= RETURN_STEP;
                        } else if (body_offset_x_ < -RETURN_STEP) {
                            body_offset_x_ += RETURN_STEP;
                        } else {
                            body_offset_x_ = 0;
                        }

                        if (head_offset_y_ > RETURN_STEP) {
                            head_offset_y_ -= RETURN_STEP;
                        } else if (head_offset_y_ < -RETURN_STEP) {
                            head_offset_y_ += RETURN_STEP;
                        } else {
                            head_offset_y_ = 0;
                        }

                        // 直接设置舵机到当前位置
                        electron_bot_.MoveSingle(90 + body_offset_x_, 4);  // BODY
                        electron_bot_.MoveSingle(90 + head_offset_y_, 5); // HEAD
                    }
                }
            } else {
                // 检测到人脸，执行平滑追踪
                RunSmoothTracking();
            }
            face_last_seen_ = false;

            vTaskDelay(pdMS_TO_TICKS(50));  // 约 20fps 追踪频率
        }
    }

    // 人脸追踪回调（由 FaceDetectReceiver 调用）
    // 机器人结构：头部舵机只能上下运动（抬头/低头），左右旋转靠身体转向舵机
    // 采用连续比例控制 + 平滑插值，实现平滑追踪
    void OnFaceTrack(bool has_face, int center_x, int center_y) {
        if (!tracking_enabled_) return;

        face_last_seen_ = has_face;
        last_face_time_ms_ = esp_timer_get_time() / 1000;

        if (!has_face) {
            return;
        }

        // 记录人脸位置，由 RunTracking 定时器连续处理
        last_face_x_ = center_x;
        last_face_y_ = center_y;
    }

    // 平滑追踪控制（由定时器任务调用）
    // 使用 MoveSingle 直接控制舵机，实现平滑连续追踪
    void RunSmoothTracking() {
        // 图像中心：320x240，中心为 (160, 120)
        const int img_center_x = 160;
        const int img_center_y = 120;

        // 计算偏移
        int delta_x = last_face_x_ - img_center_x;
        int delta_y = last_face_y_ - img_center_y;

        // 目标偏移量（比例控制）
        // 身体：delta_x 从 [-160,160] 映射到 [-BODY_MAX, BODY_MAX]
        float body_ratio = (float)BODY_MAX / 160.0f;
        int target_body_offset = (int)(-delta_x * body_ratio);
        target_body_offset = std::max(-BODY_MAX, std::min(BODY_MAX, target_body_offset));

        // 头部：delta_y 从 [-120,120] 映射到 [-HEAD_MAX_Y, HEAD_MAX_Y]
        float head_ratio = (float)HEAD_MAX_Y / 120.0f;
        int target_head_offset = (int)(-delta_y * head_ratio);
        target_head_offset = std::max(-HEAD_MAX_Y, std::min(HEAD_MAX_Y, target_head_offset));

        // 直接设置舵机到目标角度（无动画，瞬间到位）
        // servo_initial_[BODY] = 90, servo_initial_[HEAD] = 90
        int target_body_angle = 90 + target_body_offset;
        int target_head_angle = 90 + target_head_offset;
        electron_bot_.MoveSingle(target_body_angle, 4);  // BODY = 4
        electron_bot_.MoveSingle(target_head_angle, 5); // HEAD = 5

        // 更新当前偏移量记录
        body_offset_x_ = target_body_offset;
        head_offset_y_ = target_head_offset;
    }

    static void ActionTask(void* arg) {
        ElectronBotController* controller = static_cast<ElectronBotController*>(arg);
        ElectronBotActionParams params;
        controller->electron_bot_.AttachServos();

        while (true) {
            if (xQueueReceive(controller->action_queue_, &params, pdMS_TO_TICKS(1000)) == pdTRUE) {
                ESP_LOGI(TAG, "执行动作: %d", params.action_type);
                controller->is_action_in_progress_ = true;  // 开始执行动作

                // 执行相应的动作
                if (params.action_type >= ACTION_HAND_LEFT_UP &&
                    params.action_type <= ACTION_HAND_BOTH_FLAP) {
                    // 手部动作
                    controller->electron_bot_.HandAction(params.action_type, params.steps,
                                                         params.amount, params.speed);
                } else if (params.action_type >= ACTION_BODY_TURN_LEFT &&
                           params.action_type <= ACTION_BODY_TURN_CENTER) {
                    // 身体动作
                    int body_direction = params.action_type - ACTION_BODY_TURN_LEFT + 1;
                    controller->electron_bot_.BodyAction(body_direction, params.steps,
                                                         params.amount, params.speed);
                } else if (params.action_type >= ACTION_HEAD_UP &&
                           params.action_type <= ACTION_HEAD_NOD_REPEAT) {
                    // 头部动作
                    int head_action = params.action_type - ACTION_HEAD_UP + 1;
                    controller->electron_bot_.HeadAction(head_action, params.steps, params.amount,
                                                         params.speed);
                } else if (params.action_type == ACTION_HOME) {
                    // 复位动作
                    controller->electron_bot_.Home(true);
                } else if (params.action_type == ACTION_FACE_GREET) {
                    // ─── 人员识别问候序列（只打招呼，不举手） ────────────────────────

                    // 步骤1: 显示问候表情
                    auto& app = Application::GetInstance();
                    app.Schedule([]() {
                        auto display = Board::GetInstance().GetDisplay();
                        if (display) {
                            display->SetEmotion("happy");
                        }
                    });

                    // 步骤2: 触发小智唤醒，传入个性化问候语
                    const std::string greeting = controller->pending_greeting_;
                    ESP_LOGI(TAG, "触发小智唤醒：WakeWordInvoke(\"%s\")", greeting.c_str());
                    app.WakeWordInvoke(greeting);

                    // 等待小智 TTS 回复完成
                    vTaskDelay(pdMS_TO_TICKS(3000));

                    // 步骤5：恢复屏幕表情
                    app.Schedule([]() {
                        auto display = Board::GetInstance().GetDisplay();
                        if (display) {
                            display->SetEmotion("neutral");
                        }
                    });

                    ESP_LOGI(TAG, "问候序列完成：举右手 → \"%s\" → 放右手", greeting.c_str());
                } else if (params.action_type >= ACTION_FACE_TRACK_HEAD_LEFT &&
                           params.action_type <= ACTION_FACE_TRACK_HOME) {
                    // ─── 人脸追踪动作序列 ─────────────────────────────────────
                    int track_action = params.action_type - ACTION_FACE_TRACK_HEAD_LEFT + 1;
                    int amount = params.amount;

                    switch (params.action_type) {
                        case ACTION_FACE_TRACK_HEAD_LEFT:
                            // 头部左转
                            controller->electron_bot_.HeadAction(2, 1, amount, params.speed);  // 2=左转
                            break;
                        case ACTION_FACE_TRACK_HEAD_RIGHT:
                            // 头部右转
                            controller->electron_bot_.HeadAction(3, 1, amount, params.speed);  // 3=右转
                            break;
                        case ACTION_FACE_TRACK_HEAD_UP:
                            // 头部上抬
                            controller->electron_bot_.HeadAction(1, 1, amount, params.speed);  // 1=抬头
                            break;
                        case ACTION_FACE_TRACK_HEAD_DOWN:
                            // 头部下低
                            controller->electron_bot_.HeadAction(4, 1, amount, params.speed);  // 4=低头
                            break;
                        case ACTION_FACE_TRACK_BODY_LEFT:
                            // 身体左转
                            controller->electron_bot_.BodyAction(1, 1, amount, params.speed);
                            break;
                        case ACTION_FACE_TRACK_BODY_RIGHT:
                            // 身体右转
                            controller->electron_bot_.BodyAction(2, 1, amount, params.speed);
                            break;
                        case ACTION_FACE_TRACK_HOME:
                            // 回中心
                            controller->electron_bot_.HeadAction(6, 1, 0, params.speed);  // 6=回中心
                            controller->electron_bot_.BodyAction(3, 1, 0, params.speed);  // 3=回中心
                            break;
                    }
                }
                controller->is_action_in_progress_ = false;  // 动作执行完毕
            }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }

    void QueueAction(int action_type, int steps, int speed, int direction, int amount) {
        ESP_LOGI(TAG, "动作控制: 类型=%d, 步数=%d, 速度=%d, 方向=%d, 幅度=%d", action_type, steps,
                 speed, direction, amount);

        ElectronBotActionParams params = {action_type, steps, speed, direction, amount};
        xQueueSend(action_queue_, &params, portMAX_DELAY);
        StartActionTaskIfNeeded();
    }

    void StartActionTaskIfNeeded() {
        if (action_task_handle_ == nullptr) {
            xTaskCreate(ActionTask, "electron_bot_action", 1024 * 4, this, configMAX_PRIORITIES - 1,
                        &action_task_handle_);
        }
    }

    void LoadTrimsFromNVS() {
        Settings settings("electron_trims", false);

        int right_pitch = settings.GetInt("right_pitch", 0);
        int right_roll = settings.GetInt("right_roll", 0);
        int left_pitch = settings.GetInt("left_pitch", 0);
        int left_roll = settings.GetInt("left_roll", 0);
        int body = settings.GetInt("body", 0);
        int head = settings.GetInt("head", 0);
        electron_bot_.SetTrims(right_pitch, right_roll, left_pitch, left_roll, body, head);
    }

public:
    ElectronBotController() {
        electron_bot_.Init(Right_Pitch_Pin, Right_Roll_Pin, Left_Pitch_Pin, Left_Roll_Pin, Body_Pin,
                           Head_Pin);

        LoadTrimsFromNVS();
        action_queue_ = xQueueCreate(10, sizeof(ElectronBotActionParams));

        QueueAction(ACTION_HOME, 1, 1000, 0, 0);

        RegisterMcpTools();

        // 启动人员识别 UART 接收器（v2：颜色 + 性别 + 去重 + 追踪）
        // 当 Seeed XIAO ESP32S3 Sense 识别到新人员时：
        //   - 机器人举起右手（视觉提示）
        //   - 调用 WakeWordInvoke(greeting_text) 触发个性化问候
        //     greeting_text 例如："你好蓝色的小哥"
        //   - 去重逻辑在 Seeed 端（30s 冷却），ElectronBot 端不需要额外判断
        //   - 同时启用人脸追踪功能
        face_detect_receiver_.Start(
            // 打招呼回调
            [this](const PersonInfo& info, const std::string& greeting_text) {
                ESP_LOGI(TAG, "人员识别通知: pid=%d color=%s gender=%s greeting=\"%s\"",
                         info.person_id,
                         info.color.c_str(),
                         info.gender.c_str(),
                         greeting_text.c_str());

                // 将问候语存入队列（通过 action_type=ACTION_FACE_GREET）
                // greeting 文本通过线程安全的共享字符串传递
                {
                    pending_greeting_ = greeting_text;
                }
                QueueAction(ACTION_FACE_GREET, info.person_id, 800, 0, 0);
            },
            // 追踪回调
            [this](bool has_face, int center_x, int center_y) {
                OnFaceTrack(has_face, center_x, center_y);
            });

        // 启动人脸追踪任务
        xTaskCreatePinnedToCore(&TrackingTask, "face_track", 4096, this, 3, &tracking_task_handle_, 0);

        ESP_LOGI(TAG, "Electron Bot控制器已初始化并注册MCP工具（人员识别问候 v2 已启用）");
    }

    void RegisterMcpTools() {
        auto& mcp_server = McpServer::GetInstance();

        ESP_LOGI(TAG, "开始注册Electron Bot MCP工具...");

        // 手部动作统一工具
        mcp_server.AddTool(
            "self.electron.hand_action",
            "手部动作控制。action: 1=举手, 2=放手, 3=挥手, 4=拍打; hand: 1=左手, 2=右手, 3=双手; "
            "steps: 动作重复次数(1-10); speed: 动作速度(500-1500，数值越小越快); amount: "
            "动作幅度(10-50，仅举手动作使用)",
            PropertyList({Property("action", kPropertyTypeInteger, 1, 1, 4),
                          Property("hand", kPropertyTypeInteger, 3, 1, 3),
                          Property("steps", kPropertyTypeInteger, 1, 1, 10),
                          Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                          Property("amount", kPropertyTypeInteger, 30, 10, 50)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int action_type = properties["action"].value<int>();
                int hand_type = properties["hand"].value<int>();
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                int amount = properties["amount"].value<int>();

                // 根据动作类型和手部类型计算具体动作
                int base_action;
                switch (action_type) {
                    case 1:
                        base_action = ACTION_HAND_LEFT_UP;
                        break;  // 举手
                    case 2:
                        base_action = ACTION_HAND_LEFT_DOWN;
                        amount = 0;
                        break;  // 放手
                    case 3:
                        base_action = ACTION_HAND_LEFT_WAVE;
                        amount = 0;
                        break;  // 挥手
                    case 4:
                        base_action = ACTION_HAND_LEFT_FLAP;
                        amount = 0;
                        break;  // 拍打
                    default:
                        base_action = ACTION_HAND_LEFT_UP;
                }
                int action_id = base_action + (hand_type - 1);

                QueueAction(action_id, steps, speed, 0, amount);
                return true;
            });

        // 身体动作
        mcp_server.AddTool(
            "self.electron.body_turn",
            "身体转向。steps: 转向步数(1-10); speed: 转向速度(500-1500，数值越小越快); direction: "
            "转向方向(1=左转, 2=右转, 3=回中心); angle: 转向角度(0-90度)",
            PropertyList({Property("steps", kPropertyTypeInteger, 1, 1, 10),
                          Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                          Property("direction", kPropertyTypeInteger, 1, 1, 3),
                          Property("angle", kPropertyTypeInteger, 45, 0, 90)}),
            [this](const PropertyList& properties) -> ReturnValue {
                int steps = properties["steps"].value<int>();
                int speed = properties["speed"].value<int>();
                int direction = properties["direction"].value<int>();
                int amount = properties["angle"].value<int>();

                int action;
                switch (direction) {
                    case 1:
                        action = ACTION_BODY_TURN_LEFT;
                        break;
                    case 2:
                        action = ACTION_BODY_TURN_RIGHT;
                        break;
                    case 3:
                        action = ACTION_BODY_TURN_CENTER;
                        break;
                    default:
                        action = ACTION_BODY_TURN_LEFT;
                }

                QueueAction(action, steps, speed, 0, amount);
                return true;
            });

        // 头部动作
        mcp_server.AddTool("self.electron.head_move",
                           "头部运动。action: 1=抬头, 2=低头, 3=点头, 4=回中心, 5=连续点头; steps: "
                           "动作重复次数(1-10); speed: 动作速度(500-1500，数值越小越快); angle: "
                           "头部转动角度(1-15度)",
                           PropertyList({Property("action", kPropertyTypeInteger, 3, 1, 5),
                                         Property("steps", kPropertyTypeInteger, 1, 1, 10),
                                         Property("speed", kPropertyTypeInteger, 1000, 500, 1500),
                                         Property("angle", kPropertyTypeInteger, 5, 1, 15)}),
                           [this](const PropertyList& properties) -> ReturnValue {
                               int action_num = properties["action"].value<int>();
                               int steps = properties["steps"].value<int>();
                               int speed = properties["speed"].value<int>();
                               int amount = properties["angle"].value<int>();
                               int action = ACTION_HEAD_UP + (action_num - 1);
                               QueueAction(action, steps, speed, 0, amount);
                               return true;
                           });

        // 系统工具
        mcp_server.AddTool("self.electron.stop", "立即停止", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               // 清空队列但保持任务常驻
                               xQueueReset(action_queue_);
                               is_action_in_progress_ = false;
                               QueueAction(ACTION_HOME, 1, 1000, 0, 0);
                               return true;
                           });

        mcp_server.AddTool("self.electron.get_status", "获取机器人状态，返回 moving 或 idle",
                           PropertyList(), [this](const PropertyList& properties) -> ReturnValue {
                               return is_action_in_progress_ ? "moving" : "idle";
                           });

        // 单个舵机校准工具
        mcp_server.AddTool(
            "self.electron.set_trim",
            "校准单个舵机位置。设置指定舵机的微调参数以调整ElectronBot的初始姿态，设置将永久保存。"
            "servo_type: 舵机类型(right_pitch:右臂旋转, right_roll:右臂推拉, left_pitch:左臂旋转, "
            "left_roll:左臂推拉, body:身体, head:头部); "
            "trim_value: 微调值(-30到30度)",
            PropertyList({Property("servo_type", kPropertyTypeString, "right_pitch"),
                          Property("trim_value", kPropertyTypeInteger, 0, -30, 30)}),
            [this](const PropertyList& properties) -> ReturnValue {
                std::string servo_type = properties["servo_type"].value<std::string>();
                int trim_value = properties["trim_value"].value<int>();

                ESP_LOGI(TAG, "设置舵机微调: %s = %d度", servo_type.c_str(), trim_value);

                // 获取当前所有微调值
                Settings settings("electron_trims", true);
                int right_pitch = settings.GetInt("right_pitch", 0);
                int right_roll = settings.GetInt("right_roll", 0);
                int left_pitch = settings.GetInt("left_pitch", 0);
                int left_roll = settings.GetInt("left_roll", 0);
                int body = settings.GetInt("body", 0);
                int head = settings.GetInt("head", 0);

                // 更新指定舵机的微调值
                if (servo_type == "right_pitch") {
                    right_pitch = trim_value;
                    settings.SetInt("right_pitch", right_pitch);
                } else if (servo_type == "right_roll") {
                    right_roll = trim_value;
                    settings.SetInt("right_roll", right_roll);
                } else if (servo_type == "left_pitch") {
                    left_pitch = trim_value;
                    settings.SetInt("left_pitch", left_pitch);
                } else if (servo_type == "left_roll") {
                    left_roll = trim_value;
                    settings.SetInt("left_roll", left_roll);
                } else if (servo_type == "body") {
                    body = trim_value;
                    settings.SetInt("body", body);
                } else if (servo_type == "head") {
                    head = trim_value;
                    settings.SetInt("head", head);
                } else {
                    return "错误：无效的舵机类型，请使用: right_pitch, right_roll, left_pitch, "
                           "left_roll, body, head";
                }

                electron_bot_.SetTrims(right_pitch, right_roll, left_pitch, left_roll, body, head);

                QueueAction(ACTION_HOME, 1, 500, 0, 0);

                return "舵机 " + servo_type + " 微调设置为 " + std::to_string(trim_value) +
                       " 度，已永久保存";
            });

        mcp_server.AddTool("self.electron.get_trims", "获取当前的舵机微调设置", PropertyList(),
                           [this](const PropertyList& properties) -> ReturnValue {
                               Settings settings("electron_trims", false);

                               int right_pitch = settings.GetInt("right_pitch", 0);
                               int right_roll = settings.GetInt("right_roll", 0);
                               int left_pitch = settings.GetInt("left_pitch", 0);
                               int left_roll = settings.GetInt("left_roll", 0);
                               int body = settings.GetInt("body", 0);
                               int head = settings.GetInt("head", 0);

                               std::string result =
                                   "{\"right_pitch\":" + std::to_string(right_pitch) +
                                   ",\"right_roll\":" + std::to_string(right_roll) +
                                   ",\"left_pitch\":" + std::to_string(left_pitch) +
                                   ",\"left_roll\":" + std::to_string(left_roll) +
                                   ",\"body\":" + std::to_string(body) +
                                   ",\"head\":" + std::to_string(head) + "}";

                               ESP_LOGI(TAG, "获取微调设置: %s", result.c_str());
                               return result;
                           });

        mcp_server.AddTool("self.battery.get_level", "获取机器人电池电量和充电状态", PropertyList(),
                           [](const PropertyList& properties) -> ReturnValue {
                               auto& board = Board::GetInstance();
                               int level = 0;
                               bool charging = false;
                               bool discharging = false;
                               board.GetBatteryLevel(level, charging, discharging);

                               std::string status =
                                   "{\"level\":" + std::to_string(level) +
                                   ",\"charging\":" + (charging ? "true" : "false") + "}";
                               return status;
                           });

        ESP_LOGI(TAG, "Electron Bot MCP工具注册完成");
    }

    ~ElectronBotController() {
        tracking_enabled_ = false;
        if (tracking_task_handle_ != nullptr) {
            vTaskDelete(tracking_task_handle_);
            tracking_task_handle_ = nullptr;
        }
        if (action_task_handle_ != nullptr) {
            vTaskDelete(action_task_handle_);
            action_task_handle_ = nullptr;
        }
        vQueueDelete(action_queue_);
    }
};

static ElectronBotController* g_electron_controller = nullptr;

void InitializeElectronBotController() {
    if (g_electron_controller == nullptr) {
        g_electron_controller = new ElectronBotController();
        ESP_LOGI(TAG, "Electron Bot控制器已初始化并注册MCP工具");
    }
}
