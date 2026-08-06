/**
 * UART Bridge Client — ESP32-S3 侧
 * ==================================
 * 职责：
 *  1. 监听 UART，接收 ESP32 Bridge 发来的 IP 地址通知
 *  2. 将桥的 IP 保存到 NVS（namespace: "bt_bridge"）
 *  3. 提供 HTTP 转发接口：接收本地 HTTP 请求 → 转发给桥 → 返回桥的响应
 *
 * UART 连接：
 *   ESP32-S3 GPIO43 (UART1 TX) → ESP32  GPIO5 (UART2 RX)
 *   ESP32-S3 GPIO2  (UART1 RX) ← ESP32  GPIO4 (UART2 TX)  ← 接收 Bridge IP 通知
 *
 * 协议（Bridge → ESP32-S3）：
 *   "BRIDGE_IP:192.168.1.100\n"   — 桥告知自己的 IP
 *
 * 响应格式（HTTP 转发）：
 *   直接透传桥的原始 HTTP 响应体（JSON 文本）
 */

#pragma once

#include <string>
#include <functional>

/**
 * UART Bridge Client 单例
 */
class UartBridgeClient {
public:
    static UartBridgeClient& GetInstance();

    // 初始化：启动 UART 监听任务
    bool Initialize();

    // 获取桥的 IP（如未收到过通知则返回空字符串）
    std::string GetBridgeIp() const;

    // 是否已配置（收到过 BRIDGE_IP 通知）
    bool IsConfigured() const;

    // HTTP 转发：将 method/path + 可选 body 转发给桥
    //   timeout_ms: 超时时间
    //   返回桥的 HTTP 响应体（不含 header）；空字符串表示失败
    std::string ForwardHttp(const std::string& method,
                            const std::string& path,
                            const std::string& body = "",
                            int timeout_ms = 5000);

private:
    UartBridgeClient() = default;
    ~UartBridgeClient() = default;
    UartBridgeClient(const UartBridgeClient&) = delete;
    UartBridgeClient& operator=(const UartBridgeClient&) = delete;

    // UART 监听任务
    static void UartTask(void* arg);
    void ListenUart();

    // 从 NVS 加载桥 IP
    void LoadBridgeIpFromNvs();

    // 保存桥 IP 到 NVS
    void SaveBridgeIpToNvs(const std::string& ip);

    mutable std::string bridge_ip_;
    bool configured_ = false;
};
