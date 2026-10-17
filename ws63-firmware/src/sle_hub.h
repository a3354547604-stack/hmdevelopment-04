/*
 * Copyright (c) HiSilicon (Shanghai) Technologies Co., Ltd. 2022. All rights reserved.
 *
 * Description: SLE UUID Server module.
 */

/**
 * @defgroup SLE UUID SERVER API
 * @ingroup
 * @{
 */
#ifndef SLE_UUID_SERVER_H
#define SLE_UUID_SERVER_H

#include <stdbool.h>

#include "sle_ssap_server.h"

/* Service UUID */
#define SLE_UUID_SERVER_SERVICE        0x060B

/* Property UUID */
#define SLE_UUID_SERVER_NTF_REPORT     0x1122

/* Property Property */
#define SLE_UUID_TEST_PROPERTIES  (SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE)

/* Descriptor Property */
#define SLE_UUID_TEST_DESCRIPTOR   (SSAP_PERMISSION_READ | SSAP_PERMISSION_WRITE)

/**
 * @if Eng
 * @brief  SLE uuid server inir.
 * @attention  NULL
 * @retval ERRCODE_SLE_SUCCESS    Excute successfully
 * @retval ERRCODE_SLE_FAIL       Execute fail
 * @par Dependency:
 * @li sle_ssap_server.h
 * @else
 * @brief  SLE UUID服务器初始化。
 * @attention  NULL
 * @retval ERRCODE_SLE_SUCCESS    执行成功
 * @retval ERRCODE_SLE_FAIL       执行失败
 * @par 依赖:
 * @li sle_ssap_server.h
 * @endif
 */
errcode_t sle_uuid_server_init(void);

/**
 * @if Eng
 * @brief  send data to peer device by uuid on uuid server.
 * @attention  NULL
 * @param  [in]  value  send value.
 * @param  [in]  len    Length of send value。
 * @retval ERRCODE_SLE_SUCCESS    Excute successfully
 * @retval ERRCODE_SLE_FAIL       Execute fail
 * @par Dependency:
 * @li sle_ssap_server.h
 * @else
 * @brief  通过uuid server 发送数据给对端。
 * @attention  NULL
 * @retval ERRCODE_SLE_SUCCESS    执行成功
 * @retval ERRCODE_SLE_FAIL       执行失败
 * @par 依赖:
 * @li sle_ssap_server.h
 * @endif
 */
errcode_t sle_uuid_server_send_report_by_uuid(uint8_t *data, uint16_t len);

/**
 * @if Eng
 * @brief  send data to peer device by handle on uuid server.
 * @attention  NULL
 * @param  [in]  value  send value.
 * @param  [in]  len    Length of send value。
 * @retval ERRCODE_SLE_SUCCESS    Excute successfully
 * @retval ERRCODE_SLE_FAIL       Execute fail
 * @par Dependency:
 * @li sle_ssap_server.h
 * @else
 * @brief  通过uuid server 发送数据给对端。
 * @attention  NULL
 * @retval ERRCODE_SLE_SUCCESS    执行成功
 * @retval ERRCODE_SLE_FAIL       执行失败
 * @par 依赖:
 * @li sle_ssap_server.h
 * @endif
 */
errcode_t sle_uuid_server_send_report_by_handle(const uint8_t *data, uint8_t len);

/**
 * @brief  节点侧实际使用的发送接口（带 connect_id，不受「只有一个连接」的隐含假设限制）。
 * @note   与上面 _by_handle 的区别：node_link.c 需要显式指定连接，故保留这个带 conn_id 的版本。
 */
errcode_t sle_uuid_server_send_report_by_handle_id(uint8_t *data, uint16_t len, uint16_t connect_id);

/**
 * @brief  当前星闪连接号。
 * @note   ⚠ 不能拿返回值是否为 0 判断「有没有连接」：本 SDK 首条连接的 conn_id 就是 0x0000
 *         （t12 双板实测）。判断连接请用 sle_node_is_connected()。
 */
uint16_t sle_node_conn_id(void);

/**
 * @brief  是否已有一条有效星闪连接（唯一权威判断，来自 connect_state_changed 回调）。
 */
bool sle_node_is_connected(void);

void sle_enable_server_cbk(void);
#endif
