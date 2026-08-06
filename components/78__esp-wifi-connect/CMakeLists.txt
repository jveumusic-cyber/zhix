idf_component_register(
    SRCS
        "wifi_manager.cc"
        "wifi_configuration_ap.cc"
        "wifi_station.cc"
        "ssid_manager.cc"
        "dns_server.cc"
    INCLUDE_DIRS
        "include"
    EMBED_TXTFILES
        "assets/wifi_configuration.html"
        "assets/wifi_configuration_done.html"
    REQUIRES
    	"esp_timer"
        "esp_http_server"
        "esp_wifi"
        "nvs_flash"
        "json"
        "bt"
        "main"
)

# 确保 CONFIG_BT_A2DP_ENABLE 宏被传给编译器（ESP-IDF cmake 有时不稳定地处理无 Kconfig 定义的配置项）
# 在 ESP-IDF 5.x 中，即使 sdkconfig 有 CONFIG_BT_A2DP_ENABLE=y，
# 如果该选项没有对应的 Kconfig 定义，cmake 可能不会自动传递 -DCONFIG_BT_A2DP_ENABLE
# 此处显式添加编译定义，确保 wifi_configuration_ap.cc 中的 #ifdef 能正确工作
if(CONFIG_BT_A2DP_ENABLE)
    target_compile_definitions(${COMPONENT_LIB} PUBLIC CONFIG_BT_A2DP_ENABLE=1)
endif()

# 如果 electron-bot 板被选中，将其目录加到 include 路径，供蓝牙 A2DP 头文件可见
if(CONFIG_BOARD_TYPE_ELECTRON_BOT AND CONFIG_BT_A2DP_ENABLE)
    target_include_directories(${COMPONENT_LIB} PRIVATE
        "${CMAKE_SOURCE_DIR}/main/boards/electron-bot"
    )
endif()
