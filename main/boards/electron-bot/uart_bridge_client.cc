#include "uart_bridge_client.h"
#include "sdkconfig.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <esp_log.h>
#include <esp_err.h>
#include <driver/uart.h>
#include <driver/gpio.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <lwip/netdb.h>
#include <lwip/sockets.h>

#define TAG "UartBridgeClient"

// UART 配置（监听 Bridge 发来的 IP 通知）
// ESP32-S3 UART1 TX=GPIO43, RX=GPIO1（默认调试 UART）
// 用 UART1 专门收桥的消息，避免干扰日志
static constexpr uart_port_t UART_BRIDGE_NUM    = UART_NUM_1;
static constexpr int UART_BRIDGE_TX_PIN  = GPIO_NUM_43;  // 不打印，只配置
static constexpr int UART_BRIDGE_RX_PIN  = GPIO_NUM_2;    // 接收桥发来的 IP 通知
static constexpr int UART_BRIDGE_BAUD    = 115200;
static constexpr size_t UART_RX_BUF_SIZE = 256;

// NVS
static constexpr char NVS_NS[]   = "bt_bridge";
static constexpr char NVS_KEY_IP[] = "bridge_ip";

// ─────────────────────────────────────────────
//  单例
// ─────────────────────────────────────────────

UartBridgeClient& UartBridgeClient::GetInstance() {
    static UartBridgeClient instance;
    return instance;
}

// ─────────────────────────────────────────────
//  初始化
// ─────────────────────────────────────────────

bool UartBridgeClient::Initialize() {
    // 1. 从 NVS 加载桥 IP
    LoadBridgeIpFromNvs();

    // 2. 配置 UART（仅 RX，用于监听桥的 IP 通知）
    uart_config_t uart_cfg = {
        .baud_rate  = UART_BRIDGE_BAUD,
        .data_bits  = UART_DATA_8_BITS,
        .parity     = UART_PARITY_DISABLE,
        .stop_bits  = UART_STOP_BITS_1,
        .flow_ctrl  = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(UART_BRIDGE_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(UART_BRIDGE_NUM,
                                  UART_BRIDGE_TX_PIN,  // 不打印，只配置
                                  UART_BRIDGE_RX_PIN,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    // 只启用 RX buffer
    ESP_ERROR_CHECK(uart_driver_install(UART_BRIDGE_NUM,
                                        UART_RX_BUF_SIZE,  // RX ring buf
                                        0,                  // TX ring buf（不用）
                                        10,                 // queue size
                                        nullptr,             // queue ptr
                                        0));                 // intr alloc flags

    // 3. 启动 UART 监听任务
    xTaskCreatePinnedToCore(&UartBridgeClient::UartTask, "uart_bridge",
                             4096, this, 5, nullptr, 0);

    ESP_LOGI(TAG, "UART bridge client initialized (RX=GPIO%d), bridge_ip=%s",
             UART_BRIDGE_RX_PIN, bridge_ip_.c_str());
    return true;
}

// ─────────────────────────────────────────────
//  NVS
// ─────────────────────────────────────────────

void UartBridgeClient::LoadBridgeIpFromNvs() {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READONLY, &nvs) != ESP_OK) return;
    char ip_buf[16] = {};
    size_t len = sizeof(ip_buf);
    if (nvs_get_str(nvs, NVS_KEY_IP, ip_buf, &len) == ESP_OK) {
        bridge_ip_ = ip_buf;
        configured_ = !bridge_ip_.empty();
        ESP_LOGI(TAG, "Loaded bridge IP from NVS: %s", bridge_ip_.c_str());
    }
    nvs_close(nvs);
}

void UartBridgeClient::SaveBridgeIpToNvs(const std::string& ip) {
    nvs_handle_t nvs;
    if (nvs_open(NVS_NS, NVS_READWRITE, &nvs) != ESP_OK) return;
    nvs_set_str(nvs, NVS_KEY_IP, ip.c_str());
    nvs_commit(nvs);
    nvs_close(nvs);
    ESP_LOGI(TAG, "Saved bridge IP to NVS: %s", ip.c_str());
}

// ─────────────────────────────────────────────
//  查询
// ─────────────────────────────────────────────

std::string UartBridgeClient::GetBridgeIp() const {
    return bridge_ip_;
}

bool UartBridgeClient::IsConfigured() const {
    return configured_;
}

// ─────────────────────────────────────────────
//  UART 监听任务：解析 "BRIDGE_IP:x.x.x.x\n"
// ─────────────────────────────────────────────

void UartBridgeClient::UartTask(void* arg) {
    static_cast<UartBridgeClient*>(arg)->ListenUart();
    vTaskDelete(nullptr);
}

void UartBridgeClient::ListenUart() {
    uint8_t buf[UART_RX_BUF_SIZE];
    std::string line;

    while (true) {
        int len = uart_read_bytes(UART_BRIDGE_NUM, buf, sizeof(buf) - 1, pdMS_TO_TICKS(500));
        if (len > 0) {
            buf[len] = '\0';
            for (int i = 0; i < len; i++) {
                char c = static_cast<char>(buf[i]);
                if (c == '\n' || c == '\r') {
                    if (!line.empty()) {
                        // 查找 "BRIDGE_IP:xxx.xxx.xxx.xxx"
                        const char* prefix = "BRIDGE_IP:";
                        if (line.rfind(prefix, 0) == 0) {
                            std::string ip = line.substr(strlen(prefix));
                            // 简单验证：包含 "." 且长度合理
                            if (ip.find('.') != std::string::npos && ip.length() >= 7) {
                                if (ip != bridge_ip_) {
                                    bridge_ip_ = ip;
                                    configured_ = true;
                                    SaveBridgeIpToNvs(ip);
                                    ESP_LOGI(TAG, "Bridge IP updated: %s", ip.c_str());
                                }
                            }
                        }
                        line.clear();
                    }
                } else {
                    line.push_back(c);
                }
            }
        }
    }
}

// ─────────────────────────────────────────────
//  HTTP 转发（socket 实现，不依赖 esp_http_client）
// ─────────────────────────────────────────────

std::string UartBridgeClient::ForwardHttp(const std::string& method,
                                           const std::string& path,
                                           const std::string& body,
                                           int timeout_ms) {
    if (bridge_ip_.empty()) {
        ESP_LOGW(TAG, "Bridge IP not configured");
        return "";
    }

    int port = 80;

    struct hostent* he = gethostbyname(bridge_ip_.c_str());
    if (!he) {
        ESP_LOGE(TAG, "DNS lookup failed for %s", bridge_ip_.c_str());
        return "";
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket() failed");
        return "";
    }

    // 设置超时
    struct timeval tv;
    tv.tv_sec  = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port   = htons((uint16_t)port);
    memcpy(&addr.sin_addr, he->h_addr_list[0], he->h_length);

    if (connect(sock, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "connect() to %s:%d failed", bridge_ip_.c_str(), port);
        close(sock);
        return "";
    }

    // 构造 HTTP/1.1 请求
    std::string req = method + " " + path + " HTTP/1.1\r\n"
                      "Host: " + bridge_ip_ + ":" + std::to_string(port) + "\r\n"
                      "Connection: close\r\n";
    if (!body.empty()) {
        char content_len[32];
        snprintf(content_len, sizeof(content_len), "Content-Length: %zu\r\n", body.size());
        req += content_len;
    }
    req += "\r\n";
    if (!body.empty()) {
        req += body;
    }

    if (send(sock, req.c_str(), req.size(), 0) < 0) {
        ESP_LOGE(TAG, "send() failed");
        close(sock);
        return "";
    }

    // 接收响应
    std::string response;
    char rx_buf[512];
    int total = 0;
    while (true) {
        int n = recv(sock, rx_buf, sizeof(rx_buf) - 1, 0);
        if (n <= 0) break;
        rx_buf[n] = '\0';
        response.append(rx_buf, n);
        total += n;
        if (total > 1024 * 64) break;  // 防止内存溢出
    }
    close(sock);

    // 跳过 HTTP header，返回 body
    size_t body_start = response.find("\r\n\r\n");
    if (body_start != std::string::npos) {
        return response.substr(body_start + 4);
    }
    return response;
}
