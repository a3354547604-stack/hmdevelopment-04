#===============================================================================
# 自研固件：WS63 节点骨架（sensor/radar/door + 星闪 SLE 服务端 + JSON 上报）
#
# 注意：本文件是「源文件清单的镜像」，**不是**编译入口。真正编译的是 SDK 扩展点
#       src/application/samples/custom/（由 samples/CMakeLists.txt 的
#       add_subdirectory_if_exist(custom) 无条件引入），清单由
#       ws63-firmware/scripts/node-build.sh 按 scripts/node-targets.txt 生成。
#
# 下面的清单 = **M1 厨房节点（默认角色）**，与 node_cfg.h 里 NODE_ROLE 的默认值一致，
# 这样「直接拷 src/ 进 custom/ 再跑一次官方构建」也能得到一个能用的固件。
# M2/M4/mock 三个目标的清单见 node-targets.txt（区别只在驱动文件：雷达/门磁/mock）。
#===============================================================================

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/node_main.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/node_json.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/node_link.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/node_sensor_gas.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sle_hub.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/sle_hub_adv.c"
    PARENT_SCOPE)

# 不在清单里但保留在目录中的文件，以及原因：
#   sensor_hub.c / wifi_hub.c  —— 早期诊断固件（ADC/GPIO/UART/WiFi 扫描）。各自带 app_run，
#                                 编进来会多起两个任务刷屏。保留文件不动，需要时单个加回清单。
#   node_sensor_radar.c        —— M2 用（还要带上 radar_hub.c），见 node-targets.txt。
#   node_sensor_door.c         —— M4 用。
#   node_sensor_mock.c         —— mock 目标用（不碰任何硬件）。
#   node_json_selftest.c 一类   —— 主机自测用，绝不能进固件。
