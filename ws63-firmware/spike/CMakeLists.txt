#===============================================================================
# 双板星闪（SLE）连通性冒烟测试 · 组件清单
#
# 这个文件是 tools/spike-build.sh 在编译冒烟包时临时覆盖到 SDK 的
#   <sdk>/src/application/samples/custom/CMakeLists.txt
# （该目录由 samples/CMakeLists.txt 里的 add_subdirectory_if_exist(custom) 无条件引入）
#
# 与生产固件清单的差别：这里**只**列冒烟包自己的 3 个 .c，不带 sensor_hub / sle_hub /
# wifi_hub / radar_hub —— 生产 sle_hub.c 自己也会 app_run 一个 SLE server，混进来会同时
# 起两个服务端，连通性实验立刻变成不可解释的乱局。脚本编译完会把生产清单原样还原。
#===============================================================================

set(SOURCES "${SOURCES}"
    "${CMAKE_CURRENT_SOURCE_DIR}/spike_server.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/spike_server_adv.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/spike_client.c"
    PARENT_SCOPE)
