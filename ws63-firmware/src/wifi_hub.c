/**
 * WS63 自研固件 —— WiFi 能力验证（只扫描，不连接）
 *
 * 目的：验证自研固件里 WiFi 协议栈能不能起来。
 *       不需要路由器密码，扫到周围热点就说明 WiFi 通了。
 *
 * 依据（官方样例 application/samples/wifi/sta_sample/sta_sample.c）：
 *   wifi_register_event_cb() -> 等 wifi_is_wifi_inited() -> wifi_sta_enable()
 *   -> wifi_sta_scan() -> 事件回调 wifi_event_scan_state_changed 触发
 *   -> wifi_sta_get_scan_info() 取列表
 */

#include "soc_osal.h"
#include "osal_addr.h"
#include "securec.h"
#include "common_def.h"
#include "app_init.h"
#include "wifi_device.h"
#include "wifi_event.h"
#include "wifi_scan_info.h"

#include <stdio.h>

#define WIFI_HUB_TASK_PRIO       23
#define WIFI_HUB_TASK_STACK      0x3000
#define WIFI_HUB_SCAN_MAX        64

static volatile int g_wifi_scan_done = 0;
static volatile int g_wifi_scan_size = 0;

static void wifi_hub_scan_state_changed(int32_t state, int32_t size)
{
    osal_printk("[WIFI] scan_state_changed: state=%d size=%d\r\n", (int)state, (int)size);
    g_wifi_scan_size = (int)size;
    g_wifi_scan_done = 1;
}

static void wifi_hub_connection_changed(int32_t state, const wifi_linked_info_stru *info, int32_t reason_code)
{
    unused(info);
    osal_printk("[WIFI] connection_changed: state=%d reason=%d\r\n", (int)state, (int)reason_code);
}

static wifi_event_stru g_wifi_event_cb = {
    .wifi_event_connection_changed = wifi_hub_connection_changed,
    .wifi_event_scan_state_changed = wifi_hub_scan_state_changed,
};

static void wifi_hub_dump_scan(void)
{
    wifi_scan_info_stru *result = NULL;
    uint32_t num = WIFI_HUB_SCAN_MAX;   /* 入参：缓冲区容量；出参：实际条数 */
    uint32_t i;
    uint32_t len = (uint32_t)sizeof(wifi_scan_info_stru) * WIFI_HUB_SCAN_MAX;

    result = (wifi_scan_info_stru *)osal_kmalloc(len, OSAL_GFP_ATOMIC);
    if (result == NULL) {
        osal_printk("[WIFI] kmalloc(%u) for scan result FAIL\r\n", (unsigned)len);
        return;
    }
    (void)memset_s(result, len, 0, len);

    if (wifi_sta_get_scan_info(result, &num) != 0) {
        osal_printk("[WIFI] wifi_sta_get_scan_info FAIL\r\n");
        osal_kfree(result);
        return;
    }

    osal_printk("[WIFI] ========== found %u AP ==========\r\n", (unsigned)num);
    for (i = 0; i < num && i < WIFI_HUB_SCAN_MAX; i++) {
        osal_printk("[WIFI] #%u  ssid=\"%s\"  rssi=%d  ch=%d  sec=%d  bssid=%02x:%02x:%02x:%02x:%02x:%02x\r\n",
                    (unsigned)(i + 1), (const char *)result[i].ssid,
                    (int)result[i].rssi, (int)result[i].channel_num, (int)result[i].security_type,
                    result[i].bssid[0], result[i].bssid[1], result[i].bssid[2],
                    result[i].bssid[3], result[i].bssid[4], result[i].bssid[5]);
    }
    if (num == 0) {
        osal_printk("[WIFI] (0 AP: no 2.4G hotspot nearby, or antenna issue)\r\n");
    }
    osal_kfree(result);
}

static void *wifi_hub_task(const char *arg)
{
    int ret;
    int waited;

    unused(arg);

    osal_msleep(3000);   /* 等系统 + 星闪起来 */
    osal_printk("[WIFI] task start\r\n");

    ret = (int)wifi_register_event_cb(&g_wifi_event_cb);
    osal_printk("[WIFI] wifi_register_event_cb ret=%d\r\n", ret);
    if (ret != 0) {
        return NULL;
    }

    waited = 0;
    while (wifi_is_wifi_inited() == 0 && waited < 200) {
        osal_msleep(100);
        waited++;
    }
    osal_printk("[WIFI] wifi_is_wifi_inited()=%d  (waited %d*100ms)\r\n",
                (int)wifi_is_wifi_inited(), waited);

    ret = (int)wifi_sta_enable();
    osal_printk("[WIFI] wifi_sta_enable ret=%d\r\n", ret);
    if (ret != 0) {
        return NULL;
    }

    osal_msleep(500);
    ret = (int)wifi_sta_scan();
    osal_printk("[WIFI] wifi_sta_scan ret=%d\r\n", ret);

    waited = 0;
    while (g_wifi_scan_done == 0 && waited < 100) {
        osal_msleep(100);
        waited++;
    }
    osal_printk("[WIFI] scan callback fired=%d, size=%d, waited %d*100ms\r\n",
                g_wifi_scan_done, g_wifi_scan_size, waited);

    wifi_hub_dump_scan();

    osal_printk("[WIFI] ===== wifi scan verification done =====\r\n");
    return NULL;
}

static void wifi_hub_entry(void)
{
    osal_task *task_handle = NULL;

    osal_kthread_lock();
    task_handle = osal_kthread_create((osal_kthread_handler)wifi_hub_task, 0,
                                      "WifiHub", WIFI_HUB_TASK_STACK);
    if (task_handle != NULL) {
        osal_kthread_set_priority(task_handle, WIFI_HUB_TASK_PRIO);
    }
    osal_kthread_unlock();
}

app_run(wifi_hub_entry);
