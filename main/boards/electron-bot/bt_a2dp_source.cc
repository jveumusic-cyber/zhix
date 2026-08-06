#include "bt_a2dp_source.h"

#ifdef CONFIG_BT_A2DP_ENABLE
#ifdef CONFIG_IDF_TARGET_ESP32  // 仅在 ESP32 上编译，ESP32-S3 不支持 Classic BT

#include <cstring>
#include <cmath>
#include <algorithm>

#include <esp_log.h>
#include <esp_bt.h>
#include <esp_bt_main.h>
#include <esp_bt_device.h>
#include <esp_gap_bt_api.h>
#include <esp_a2dp_api.h>
#include <nvs.h>
#include <nvs_flash.h>

#define TAG "BtA2dpSource"

// A2DP 输出固定参数
static constexpr int kA2dpSampleRate  = 44100;
static constexpr int kA2dpChannels    = 2;       // 立体声
static constexpr int kSrcSampleRate   = 24000;   // 小智输出采样率

BtA2dpSource& BtA2dpSource::GetInstance() {
    static BtA2dpSource instance;
    return instance;
}

// ─────────────────────────────────────────────
//  初始化 / 反初始化
// ─────────────────────────────────────────────

bool BtA2dpSource::Initialize() {
    if (initialized_) {
        ESP_LOGW(TAG, "Already initialized");
        return true;
    }

    ESP_LOGI(TAG, "Initializing Classic BT + A2DP Source");

    // 仅 ESP32 原版有双模（Classic + BLE），才能释放 BLE 内存
    // ESP32-S3 只有 BLE，不存在 Classic BT，此处跳过
#if CONFIG_IDF_TARGET_ESP32
    ESP_ERROR_CHECK(esp_bt_controller_mem_release(ESP_BT_MODE_BLE));
#endif

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_bt_controller_enable(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bt_controller_enable failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_bluedroid_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_init failed: %s", esp_err_to_name(err));
        return false;
    }

    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_bluedroid_enable failed: %s", esp_err_to_name(err));
        return false;
    }

    // 设置 GAP 回调
    ESP_ERROR_CHECK(esp_bt_gap_register_callback(GapCallback));

    // 注册 A2DP Source
    ESP_ERROR_CHECK(esp_a2d_register_callback(A2dpCallback));
    ESP_ERROR_CHECK(esp_a2d_source_register_data_callback(DataCallback));
    ESP_ERROR_CHECK(esp_a2d_source_init());

    // 设备名称
    ESP_ERROR_CHECK(esp_bt_dev_set_device_name("Xiaozhi-ElectronBot"));

    // 设置可发现/可连接
    ESP_ERROR_CHECK(esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE));

    // 初始化音频缓冲
    audio_buf_.resize(kAudioBufSamples, 0);
    buf_write_ = buf_read_ = buf_used_ = 0;

    initialized_ = true;
    state_ = State::IDLE;
    ESP_LOGI(TAG, "BtA2dpSource initialized");
    return true;
}

void BtA2dpSource::Deinitialize() {
    if (!initialized_) return;

    if (state_ == State::CONNECTED) {
        Disconnect();
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    esp_a2d_source_deinit();
    esp_bluedroid_disable();
    esp_bluedroid_deinit();
    esp_bt_controller_disable();
    esp_bt_controller_deinit();

    initialized_ = false;
    state_ = State::IDLE;
    ESP_LOGI(TAG, "BtA2dpSource deinitialized");
}

// ─────────────────────────────────────────────
//  扫描
// ─────────────────────────────────────────────

void BtA2dpSource::StartScan(int duration_seconds) {
    if (!initialized_) return;
    {
        std::lock_guard<std::mutex> lock(devices_mutex_);
        scanned_devices_.clear();
    }
    state_ = State::SCANNING;
    ESP_LOGI(TAG, "Starting BT scan for %d seconds", duration_seconds);
    esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, duration_seconds, 0);
}

void BtA2dpSource::StopScan() {
    if (!initialized_) return;
    esp_bt_gap_cancel_discovery();
    if (state_ == State::SCANNING) {
        state_ = State::IDLE;
    }
}

std::vector<BtA2dpSource::BtDevice> BtA2dpSource::GetScannedDevices() {
    std::lock_guard<std::mutex> lock(devices_mutex_);
    return scanned_devices_;
}

// ─────────────────────────────────────────────
//  连接 / 断开
// ─────────────────────────────────────────────

bool BtA2dpSource::Connect(const uint8_t mac[6]) {
    if (!initialized_) {
        ESP_LOGE(TAG, "Not initialized");
        return false;
    }
    if (state_ == State::CONNECTED || state_ == State::CONNECTING) {
        ESP_LOGW(TAG, "Already connected or connecting");
        return false;
    }
    memcpy(target_mac_, mac, 6);
    state_ = State::CONNECTING;
    ESP_LOGI(TAG, "Connecting to %02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_err_t err = esp_a2d_source_connect(const_cast<uint8_t*>(mac));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_a2d_source_connect failed: %s", esp_err_to_name(err));
        state_ = State::IDLE;
        return false;
    }
    return true;
}

bool BtA2dpSource::Connect(const std::string& mac_str) {
    uint8_t mac[6];
    if (sscanf(mac_str.c_str(), "%hhx:%hhx:%hhx:%hhx:%hhx:%hhx",
               &mac[0], &mac[1], &mac[2], &mac[3], &mac[4], &mac[5]) != 6) {
        ESP_LOGE(TAG, "Invalid MAC string: %s", mac_str.c_str());
        return false;
    }
    return Connect(mac);
}

void BtA2dpSource::Disconnect() {
    if (!initialized_ || state_ != State::CONNECTED) return;
    state_ = State::DISCONNECTING;
    esp_a2d_source_disconnect(connected_mac_);
}

// ─────────────────────────────────────────────
//  音频写入（24kHz mono → 44.1kHz stereo FIFO）
// ─────────────────────────────────────────────

int BtA2dpSource::WriteAudio(const int16_t* data, int samples) {
    if (state_ != State::CONNECTED) return samples; // 丢弃，避免阻塞

    // 重采样输出缓冲（最大 4× 输入大小）
    const int max_out = samples * 8; // 24k→44.1k ×1.8375, 再 ×2 声道 → ~3.7×
    std::vector<int16_t> resampled(max_out);
    int out_samples = 0;
    Resample24kTo44k(data, samples, resampled.data(), &out_samples);

    std::lock_guard<std::mutex> lock(buf_mutex_);
    const size_t buf_size = audio_buf_.size();
    const size_t free_space = buf_size - buf_used_;
    const size_t to_write = std::min((size_t)out_samples, free_space);

    if (to_write < (size_t)out_samples) {
        ESP_LOGW(TAG, "Audio buffer overflow, dropping %zu samples", (size_t)out_samples - to_write);
    }

    for (size_t i = 0; i < to_write; i++) {
        audio_buf_[buf_write_] = resampled[i];
        buf_write_ = (buf_write_ + 1) % buf_size;
    }
    buf_used_ += to_write;
    return samples;
}

// ─────────────────────────────────────────────
//  重采样：24kHz mono int16 → 44100Hz stereo int16
//  方法：线性插值
// ─────────────────────────────────────────────

void BtA2dpSource::Resample24kTo44k(const int16_t* src, int src_samples,
                                     int16_t* dst, int* dst_samples) {
    // 比率：输出采样数/输入采样数 = 44100/24000
    static constexpr float kRatio = (float)kA2dpSampleRate / (float)kSrcSampleRate;

    int out_idx = 0;
    for (int i = 0; i < src_samples; i++) {
        // 用 phase 控制输出密度
        float phase_end = resample_phase_ + kRatio;
        // 输出所有在 [resample_phase_, phase_end) 之间的采样
        while (resample_phase_ < (float)(i + 1)) {
            // 线性插值
            float t = resample_phase_ - (float)i;
            int16_t interp;
            if (t <= 0.0f) {
                interp = last_sample_;
            } else {
                interp = (int16_t)(last_sample_ + t * (src[i] - last_sample_));
            }
            // 立体声：左右相同
            dst[out_idx++] = interp;
            dst[out_idx++] = interp;
            resample_phase_ += 1.0f / kRatio;
        }
        last_sample_ = src[i];
    }
    // 修正 phase（避免 float 累积误差）
    resample_phase_ -= src_samples;
    if (resample_phase_ < 0.0f) resample_phase_ = 0.0f;

    *dst_samples = out_idx;
}

// ─────────────────────────────────────────────
//  A2DP 数据回调（BT 协议栈拉音频数据）
// ─────────────────────────────────────────────

int32_t BtA2dpSource::DataCallback(uint8_t* data, int32_t len) {
    auto& self = GetInstance();
    int16_t* pcm = reinterpret_cast<int16_t*>(data);
    int32_t samples_wanted = len / sizeof(int16_t);

    std::lock_guard<std::mutex> lock(self.buf_mutex_);
    int32_t available = (int32_t)self.buf_used_;
    int32_t to_copy = std::min(samples_wanted, available);

    for (int32_t i = 0; i < to_copy; i++) {
        pcm[i] = self.audio_buf_[self.buf_read_];
        self.buf_read_ = (self.buf_read_ + 1) % self.audio_buf_.size();
    }
    self.buf_used_ -= to_copy;

    // 不足时补静音
    if (to_copy < samples_wanted) {
        memset(&pcm[to_copy], 0, (samples_wanted - to_copy) * sizeof(int16_t));
    }
    return len;
}

// ─────────────────────────────────────────────
//  GAP 回调
// ─────────────────────────────────────────────

void BtA2dpSource::GapCallback(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t* param) {
    auto& self = GetInstance();

    switch (event) {
    case ESP_BT_GAP_DISC_RES_EVT: {
        // 扫描到设备
        BtDevice dev;
        memcpy(dev.mac, param->disc_res.bda, 6);
        char mac_buf[18];
        snprintf(mac_buf, sizeof(mac_buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                 dev.mac[0], dev.mac[1], dev.mac[2], dev.mac[3], dev.mac[4], dev.mac[5]);
        dev.mac_str = mac_buf;
        dev.rssi = -80;
        dev.name = dev.mac_str; // 默认用 MAC 作为名称

        for (int i = 0; i < param->disc_res.num_prop; i++) {
            auto& prop = param->disc_res.prop[i];
            if (prop.type == ESP_BT_GAP_DEV_PROP_EIR) {
                uint8_t name_len = 0;
                uint8_t* name_ptr = esp_bt_gap_resolve_eir_data(
                    (uint8_t*)prop.val, ESP_BT_EIR_TYPE_CMPL_LOCAL_NAME, &name_len);
                if (!name_ptr) {
                    name_ptr = esp_bt_gap_resolve_eir_data(
                        (uint8_t*)prop.val, ESP_BT_EIR_TYPE_SHORT_LOCAL_NAME, &name_len);
                }
                if (name_ptr && name_len > 0) {
                    dev.name = std::string((char*)name_ptr, name_len);
                }
            } else if (prop.type == ESP_BT_GAP_DEV_PROP_RSSI) {
                dev.rssi = *(int8_t*)prop.val;
            }
        }

        ESP_LOGI(TAG, "Found BT device: %s (%s) rssi=%d", dev.name.c_str(), dev.mac_str.c_str(), dev.rssi);

        std::lock_guard<std::mutex> lock(self.devices_mutex_);
        // 去重
        bool found = false;
        for (auto& d : self.scanned_devices_) {
            if (memcmp(d.mac, dev.mac, 6) == 0) {
                d = dev; found = true; break;
            }
        }
        if (!found) self.scanned_devices_.push_back(dev);
        break;
    }

    case ESP_BT_GAP_DISC_STATE_CHANGED_EVT:
        if (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STOPPED) {
            ESP_LOGI(TAG, "BT scan completed");
            if (self.state_ == State::SCANNING) {
                self.state_ = State::IDLE;
            }
        }
        break;

    default:
        break;
    }
}

// ─────────────────────────────────────────────
//  A2DP 回调
// ─────────────────────────────────────────────

void BtA2dpSource::A2dpCallback(esp_a2d_cb_event_t event, esp_a2d_cb_param_t* param) {
    auto& self = GetInstance();

    switch (event) {
    case ESP_A2D_CONNECTION_STATE_EVT: {
        auto& conn = param->conn_stat;
        if (conn.state == ESP_A2D_CONNECTION_STATE_CONNECTED) {
            memcpy(self.connected_mac_, conn.remote_bda, 6);
            // 尝试从已扫描列表找名字
            {
                std::lock_guard<std::mutex> lock(self.devices_mutex_);
                for (auto& d : self.scanned_devices_) {
                    if (memcmp(d.mac, conn.remote_bda, 6) == 0) {
                        self.connected_name_ = d.name;
                        break;
                    }
                }
            }
            if (self.connected_name_.empty()) {
                char buf[18];
                snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
                         conn.remote_bda[0], conn.remote_bda[1], conn.remote_bda[2],
                         conn.remote_bda[3], conn.remote_bda[4], conn.remote_bda[5]);
                self.connected_name_ = buf;
            }
            self.state_ = State::CONNECTED;
            ESP_LOGI(TAG, "A2DP connected to %s", self.connected_name_.c_str());
            // 持久化
            self.SavePairedDevice(self.connected_mac_, self.connected_name_);
            if (self.connection_cb_) self.connection_cb_(true, self.connected_name_);
        } else if (conn.state == ESP_A2D_CONNECTION_STATE_DISCONNECTED) {
            std::string name = self.connected_name_;
            memset(self.connected_mac_, 0, 6);
            self.connected_name_.clear();
            self.state_ = State::IDLE;
            ESP_LOGI(TAG, "A2DP disconnected from %s", name.c_str());
            if (self.connection_cb_) self.connection_cb_(false, name);
        } else if (conn.state == ESP_A2D_CONNECTION_STATE_CONNECTING) {
            self.state_ = State::CONNECTING;
        } else if (conn.state == ESP_A2D_CONNECTION_STATE_DISCONNECTING) {
            self.state_ = State::DISCONNECTING;
        }
        break;
    }

    case ESP_A2D_AUDIO_STATE_EVT:
        ESP_LOGI(TAG, "A2DP audio state: %d", param->audio_stat.state);
        break;

    case ESP_A2D_AUDIO_CFG_EVT:
        // ESP-IDF v5.x: mcc.cie 是 union，只有 type==SBC 才能解引用 sbc 字段
        ESP_LOGI(TAG, "A2DP audio config: codec_type=%d", param->audio_cfg.mcc.type);
        break;

    default:
        break;
    }
}

// ─────────────────────────────────────────────
//  状态查询
// ─────────────────────────────────────────────

BtA2dpSource::State BtA2dpSource::GetState() const { return state_; }
bool BtA2dpSource::IsConnected() const { return state_ == State::CONNECTED; }

std::string BtA2dpSource::GetConnectedDeviceName() const { return connected_name_; }

std::string BtA2dpSource::GetConnectedDeviceMac() const {
    if (!IsConnected()) return "";
    char buf[18];
    snprintf(buf, sizeof(buf), "%02X:%02X:%02X:%02X:%02X:%02X",
             connected_mac_[0], connected_mac_[1], connected_mac_[2],
             connected_mac_[3], connected_mac_[4], connected_mac_[5]);
    return buf;
}

// ─────────────────────────────────────────────
//  NVS 持久化
// ─────────────────────────────────────────────

void BtA2dpSource::SavePairedDevice(const uint8_t mac[6], const std::string& name) {
    nvs_handle_t nvs;
    if (nvs_open("bt_a2dp", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_blob(nvs, "paired_mac", mac, 6);
    nvs_set_str(nvs, "paired_name", name.c_str());
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Saved paired device: %s", name.c_str());
}

bool BtA2dpSource::LoadPairedDevice(uint8_t mac[6], std::string& name) {
    nvs_handle_t nvs;
    if (nvs_open("bt_a2dp", NVS_READONLY, &nvs) != ESP_OK) return false;

    size_t mac_size = 6;
    esp_err_t err = nvs_get_blob(nvs, "paired_mac", mac, &mac_size);
    if (err != ESP_OK) { nvs_close(nvs); return false; }

    char name_buf[64] = {};
    size_t name_size = sizeof(name_buf);
    err = nvs_get_str(nvs, "paired_name", name_buf, &name_size);
    nvs_close(nvs);
    if (err != ESP_OK) return false;

    name = name_buf;
    return true;
}

void BtA2dpSource::ClearPairedDevice() {
    nvs_handle_t nvs;
    if (nvs_open("bt_a2dp", NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_erase_key(nvs, "paired_mac");
    nvs_erase_key(nvs, "paired_name");
    nvs_commit(nvs);
    nvs_close(nvs);
}

#endif  // CONFIG_IDF_TARGET_ESP32
#endif // CONFIG_BT_A2DP_ENABLE
