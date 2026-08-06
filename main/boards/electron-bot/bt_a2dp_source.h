#pragma once

#ifdef CONFIG_BT_A2DP_ENABLE

#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>

#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>
#include <freertos/semphr.h>
#include <esp_a2dp_api.h>
#include <esp_gap_bt_api.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>

/**
 * BtA2dpSource - ESP32 Classic BT A2DP Source 管理类
 *
 * 负责：
 *  1. 初始化 Classic BT + A2DP Source
 *  2. 扫描附近蓝牙设备
 *  3. 连接/断开指定 MAC 的蓝牙音响
 *  4. 接收外部 PCM 数据并通过 A2DP 发送（内部做 24kHz->44.1kHz 重采样）
 *  5. NVS 持久化：保存最近一次成功连接的设备 MAC
 */
class BtA2dpSource {
public:
    struct BtDevice {
        std::string name;
        std::string mac_str;   // "AA:BB:CC:DD:EE:FF"
        uint8_t mac[6];
        int rssi;
    };

    enum class State {
        IDLE,           // 未初始化
        SCANNING,       // 正在扫描
        CONNECTING,     // 正在连接
        CONNECTED,      // 已连接，可输出音频
        DISCONNECTING,  // 断开中
    };

    static BtA2dpSource& GetInstance();

    // 初始化蓝牙控制器 + bluedroid + A2DP
    // 注意：必须在 WiFi 初始化之后调用
    bool Initialize();
    void Deinitialize();

    // 扫描周边蓝牙设备（异步，结果通过 GetScannedDevices() 获取）
    void StartScan(int duration_seconds = 5);
    void StopScan();
    std::vector<BtDevice> GetScannedDevices();

    // 连接 / 断开
    bool Connect(const uint8_t mac[6]);
    bool Connect(const std::string& mac_str);
    void Disconnect();

    // 音频写入（由 NoAudioCodecSimplex::Write 路由调用）
    // data: 24kHz 单声道 int16_t PCM；samples: 样本数
    // 返回实际消耗的样本数
    int WriteAudio(const int16_t* data, int samples);

    // 查询状态
    State GetState() const;
    bool IsConnected() const;
    std::string GetConnectedDeviceName() const;
    std::string GetConnectedDeviceMac() const;

    // NVS 持久化
    void SavePairedDevice(const uint8_t mac[6], const std::string& name);
    bool LoadPairedDevice(uint8_t mac[6], std::string& name);
    void ClearPairedDevice();

    // 回调
    using ConnectionCallback = std::function<void(bool connected, const std::string& device_name)>;
    void SetConnectionCallback(ConnectionCallback cb) { connection_cb_ = cb; }

private:
    BtA2dpSource() = default;
    ~BtA2dpSource() = default;
    BtA2dpSource(const BtA2dpSource&) = delete;
    BtA2dpSource& operator=(const BtA2dpSource&) = delete;

    // ESP-IDF 回调（静态）
    static void GapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param);
    static void A2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param);
    static int32_t DataCallback(uint8_t* data, int32_t len);

    // 内部重采样：24000Hz mono int16 → 44100Hz stereo int16
    void Resample24kTo44k(const int16_t* src, int src_samples,
                          int16_t* dst, int* dst_samples);

    // 状态
    std::atomic<State> state_{State::IDLE};
    bool initialized_ = false;

    // 扫描结果
    mutable std::mutex devices_mutex_;
    std::vector<BtDevice> scanned_devices_;

    // 已连接设备
    uint8_t connected_mac_[6] = {};
    std::string connected_name_;

    // 音频 FIFO（循环缓冲，供 DataCallback 拉取）
    static constexpr size_t kAudioBufSamples = 44100 * 2 * 2;  // ~2s 立体声 44100Hz
    std::vector<int16_t> audio_buf_;
    size_t buf_write_ = 0;
    size_t buf_read_  = 0;
    size_t buf_used_  = 0;
    std::mutex buf_mutex_;

    // 重采样状态（线性插值）
    float resample_phase_ = 0.0f;
    int16_t last_sample_  = 0;

    // 回调
    ConnectionCallback connection_cb_;

    // 待连接 MAC（Connect 后写入，gap_callback 里读取）
    uint8_t target_mac_[6] = {};
};

#endif // CONFIG_BT_A2DP_ENABLE
