#include <driver/i2c_master.h>
#include <driver/spi_common.h>
#include <esp_lcd_gc9a01.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_vendor.h>
#include <esp_log.h>

#include "application.h"
#include "codecs/no_audio_codec.h"
#include "button.h"
#include "config.h"
#include "display/lcd_display.h"
#include "driver/spi_master.h"
#include "electron_emoji_display.h"
#include "movements.h"
#include "power_manager.h"
#include "system_reset.h"
#include "wifi_board.h"

#ifdef CONFIG_BT_A2DP_ENABLE
#include "bt_a2dp_source.h"
#endif

#define TAG "ElectronBot"

// 控制器初始化函数声明
void InitializeElectronBotController();

// ─────────────────────────────────────────────────────────────────────────────
// BtRoutedAudioCodec：继承 NoAudioCodecSimplex，Write 时同时路由到 A2DP
// ─────────────────────────────────────────────────────────────────────────────
class BtRoutedAudioCodec : public NoAudioCodecSimplex {
public:
    BtRoutedAudioCodec(int input_sample_rate, int output_sample_rate,
                       gpio_num_t spk_bclk, gpio_num_t spk_ws, gpio_num_t spk_dout,
                       gpio_num_t mic_sck, gpio_num_t mic_ws, gpio_num_t mic_din)
        : NoAudioCodecSimplex(input_sample_rate, output_sample_rate,
                              spk_bclk, spk_ws, spk_dout,
                              mic_sck, mic_ws, mic_din) {}

protected:
    int Write(const int16_t* data, int samples) override {
#if defined(CONFIG_BT_A2DP_ENABLE) && defined(CONFIG_IDF_TARGET_ESP32)
        // ESP32: 如果蓝牙已连接，音频数据路由到 A2DP，不发到 I2S 扬声器
        auto& bt = BtA2dpSource::GetInstance();
        if (bt.IsConnected()) {
            return bt.WriteAudio(data, samples);
        }
#endif
        // 默认：写入 I2S 板载扬声器
        return NoAudioCodecSimplex::Write(data, samples);
    }
};

class ElectronBot : public WifiBoard {
private:
    Display* display_;
    PowerManager* power_manager_;
    Button boot_button_;

    void InitializePowerManager() {
        power_manager_ =
            new PowerManager(POWER_CHARGE_DETECT_PIN, POWER_ADC_UNIT, POWER_ADC_CHANNEL);
    }

    void InitializeSpi() {
        ESP_LOGI(TAG, "Initialize SPI bus");
        spi_bus_config_t buscfg =
            GC9A01_PANEL_BUS_SPI_CONFIG(DISPLAY_SPI_SCLK_PIN, DISPLAY_SPI_MOSI_PIN,
                                        DISPLAY_WIDTH * DISPLAY_HEIGHT * sizeof(uint16_t));
        ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO));
    }

    // GC9A01初始化
    void InitializeGc9a01Display() {
        ESP_LOGI(TAG, "Init GC9A01 display");

        ESP_LOGI(TAG, "Install panel IO");
        esp_lcd_panel_io_handle_t io_handle = NULL;
        esp_lcd_panel_io_spi_config_t io_config =
            GC9A01_PANEL_IO_SPI_CONFIG(DISPLAY_SPI_CS_PIN, DISPLAY_SPI_DC_PIN, NULL, NULL);
        io_config.pclk_hz = DISPLAY_SPI_SCLK_HZ;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI3_HOST, &io_config, &io_handle));

        ESP_LOGI(TAG, "Install GC9A01 panel driver");
        esp_lcd_panel_handle_t panel_handle = NULL;
        esp_lcd_panel_dev_config_t panel_config = {};
        panel_config.reset_gpio_num = DISPLAY_SPI_RESET_PIN;  // Set to -1 if not use
        panel_config.rgb_endian = LCD_RGB_ENDIAN_BGR;         // LCD_RGB_ENDIAN_RGB;
        panel_config.bits_per_pixel = 16;  // Implemented by LCD command `3Ah` (16/18)

        ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(io_handle, &panel_config, &panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_reset(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_init(panel_handle));
        ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel_handle, true));
        ESP_ERROR_CHECK(esp_lcd_panel_mirror(panel_handle, true, false));
        // Note: esp_lcd_panel_disp_on_off() is called in SpiLcdDisplay constructor (lcd_display.cc)
        // to ensure proper timing after white screen draw and before LVGL init

        display_ = new ElectronEmojiDisplay(io_handle, panel_handle, DISPLAY_WIDTH, DISPLAY_HEIGHT,
                                            DISPLAY_OFFSET_X, DISPLAY_OFFSET_Y, DISPLAY_MIRROR_X,
                                            DISPLAY_MIRROR_Y, DISPLAY_SWAP_XY);
    }

    void InitializeButtons() {
        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            app.ToggleChatState();
        });
    }

    void InitializeController() { InitializeElectronBotController(); }

#ifdef CONFIG_BT_A2DP_ENABLE
    void InitializeBluetooth() {
#ifdef CONFIG_IDF_TARGET_ESP32
        // ESP32: 初始化本地 Classic BT
        auto& bt = BtA2dpSource::GetInstance();
        // 尝试自动重连上次配对的设备
        uint8_t mac[6];
        std::string name;
        if (bt.LoadPairedDevice(mac, name)) {
            ESP_LOGI(TAG, "Trying to auto-reconnect to BT device: %s", name.c_str());
            if (bt.Initialize()) {
                bt.Connect(mac);
            }
        }
        // 连接结果回调：在 Display 上显示提示（可选）
        bt.SetConnectionCallback([](bool connected, const std::string& device_name) {
            if (connected) {
                ESP_LOGI(TAG, "BT audio output: connected to %s", device_name.c_str());
            } else {
                ESP_LOGI(TAG, "BT audio output: disconnected from %s", device_name.c_str());
            }
        });
#else
        // ESP32-S3: 蓝牙通过 Bridge 代理，此处不初始化本地 BT
        // BtRoutedAudioCodec::Write() 会检查 Bridge 连接状态自动路由音频
        ESP_LOGI(TAG, "Bluetooth: bridge-only mode (no local Classic BT on S3)");
#endif
    }
#endif

public:
    ElectronBot() : boot_button_(BOOT_BUTTON_GPIO) {
        InitializeSpi();
        InitializeGc9a01Display();
        InitializeButtons();
        InitializePowerManager();
        InitializeController();

        if (DISPLAY_BACKLIGHT_PIN != GPIO_NUM_NC) {
            GetBacklight()->RestoreBrightness();
        }

#if defined(CONFIG_BT_A2DP_ENABLE) && defined(CONFIG_IDF_TARGET_ESP32)
        // ESP32: 蓝牙初始化放在最后（WiFi 已在 WifiBoard 父类初始化中完成）
        InitializeBluetooth();
#endif
    }

    virtual AudioCodec* GetAudioCodec() override {
        static BtRoutedAudioCodec audio_codec(
            AUDIO_INPUT_SAMPLE_RATE, AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK, AUDIO_I2S_SPK_GPIO_LRCK,
            AUDIO_I2S_SPK_GPIO_DOUT, AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS, AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override { return display_; }

    virtual Backlight* GetBacklight() override {
        static PwmBacklight backlight(DISPLAY_BACKLIGHT_PIN, DISPLAY_BACKLIGHT_OUTPUT_INVERT);
        return &backlight;
    }
    virtual bool GetBatteryLevel(int& level, bool& charging, bool& discharging) override {
        charging = power_manager_->IsCharging();
        discharging = !charging;
        level = power_manager_->GetBatteryLevel();
        return true;
    }
};

DECLARE_BOARD(ElectronBot);
