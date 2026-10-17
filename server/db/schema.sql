-- server/db/schema.sql — MySQL 8 建表脚本（契约 §7 的逐列对应物；契约 §7.13 规定 DDL 由 server/ 维护）
-- 约定（§7.1）：InnoDB + utf8mb4/utf8mb4_0900_ai_ci；主键 id BIGINT UNSIGNED AUTO_INCREMENT；
--   时间列 DATETIME(3) 一律 UTC（连接初始化 SET time_zone='+00:00'），另有 *_ms 冗余毫秒列；
--   不用外键约束（应用层保证 + 索引加速）；除 user_devices 外一律物理删除。
-- 幂等：全部 IF NOT EXISTS，服务启动时重复执行安全（无迁移框架，见 README「为什么不用 alembic」）。

-- §7.2 users
CREATE TABLE IF NOT EXISTS `users` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `username`      VARCHAR(32)  NOT NULL COMMENT '登录名，全局唯一',
  `password_hash` VARCHAR(100) NOT NULL COMMENT 'bcrypt cost=10',
  `phone`         VARCHAR(20)  DEFAULT NULL COMMENT '11 位数字',
  `created_at`    DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_users_username` (`username`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='用户（App 账号）';

-- §7.3 devices
CREATE TABLE IF NOT EXISTS `devices` (
  `id`                  BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `sn`                  VARCHAR(32)  NOT NULL COMMENT '网关 SN，如 M0-3F1A2B',
  `name`                VARCHAR(64)  NOT NULL COMMENT '显示名，默认取 sn（应用层赋值：列默认值不能引用其它列）',
  `bind_code`           CHAR(6)      DEFAULT NULL COMMENT '6 位数字，绑定成功后清空',
  `bind_code_expire_at` DATETIME(3)  DEFAULT NULL COMMENT '绑定码有效期 10 分钟',
  `iotda_device_id`     VARCHAR(64)  DEFAULT NULL COMMENT 'IoTDA 平台字符串设备 ID（不是 SN）',
  `owner_id`            BIGINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '首位绑定者快照；权限判定一律查 user_devices；0=尚未绑定（预置设备）',
  `fw_ver`              VARCHAR(16)  DEFAULT NULL COMMENT '网关固件版本',
  -- ⚠️ 契约增补列（不在 §7.3 里）：契约没有为 §6.9/§6.10 的阈值配置规定存储位置。
  --    取值形如 {"items":{...},"updated_at":1760000000000,"source":"local"|"device"}；
  --    待 PM 裁决后由 architect 登记进契约 §12（新增列比新增第 12 张表更小的改动面）。
  `settings_json`       JSON         DEFAULT NULL COMMENT '阈值配置与来源（§6.9/§6.10）',
  `created_at`          DATETIME(3)  NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_devices_sn` (`sn`),
  KEY `idx_devices_iotda` (`iotda_device_id`),
  KEY `idx_devices_owner` (`owner_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='网关设备（1 台设备=1 个网关）';

-- §7.4 user_devices
CREATE TABLE IF NOT EXISTS `user_devices` (
  `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `user_id`    BIGINT UNSIGNED NOT NULL,
  `device_id`  BIGINT UNSIGNED NOT NULL,
  `role`       ENUM('owner','family') NOT NULL DEFAULT 'family' COMMENT '只有 owner 能改设置/演示/解绑',
  `deleted_at` DATETIME(3) DEFAULT NULL COMMENT '软删除（解绑）',
  `created_at` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_user_devices` (`user_id`,`device_id`),
  KEY `idx_user_devices_device` (`device_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='用户↔设备关系（权限唯一判据）';

-- §7.5 nodes
CREATE TABLE IF NOT EXISTS `nodes` (
  `id`           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`    BIGINT UNSIGNED NOT NULL,
  `sn`           VARCHAR(16) NOT NULL COMMENT 'H1/M1/M2/M3/M4/M5',
  `type`         VARCHAR(16) NOT NULL COMMENT 'hub/gas/fall/door/presence',
  `location`     VARCHAR(32) DEFAULT NULL COMMENT '房间',
  `online`       TINYINT(1)  NOT NULL DEFAULT 0,
  `last_seen`    DATETIME(3) DEFAULT NULL,
  `last_seen_ms` BIGINT UNSIGNED DEFAULT NULL,
  `rssi`         SMALLINT    DEFAULT NULL COMMENT 'dBm',
  `battery`      TINYINT UNSIGNED DEFAULT NULL COMMENT '0-100，255=市电',
  `updated_at`   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_nodes_sn` (`device_id`,`sn`),
  KEY `idx_nodes_online` (`device_id`,`online`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='传感器节点在线表';

-- §7.6 sensor_readings
CREATE TABLE IF NOT EXISTS `sensor_readings` (
  `id`           BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`    BIGINT UNSIGNED NOT NULL,
  `node_sn`      VARCHAR(16) NOT NULL COMMENT '采集节点；网关属性快照用 M0',
  `type`         VARCHAR(16) NOT NULL COMMENT 'gas/fall/door/presence/status/env + gateway',
  `ts`           DATETIME(3) NOT NULL COMMENT '采样时刻（UTC）',
  `ts_ms`        BIGINT UNSIGNED NOT NULL COMMENT '原始毫秒（去重键之一）',
  `payload_json` JSON        NOT NULL,
  `created_at`   DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`id`),
  -- 该唯一键同时充当 webhook 重试的幂等键（§7.6）
  UNIQUE KEY `uk_readings` (`device_id`,`node_sn`,`type`,`ts_ms`),
  KEY `idx_readings_lookup` (`device_id`,`type`,`ts`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='传感器读数（报表归并数据源）';

-- §7.7 alerts
CREATE TABLE IF NOT EXISTS `alerts` (
  `id`            BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`     BIGINT UNSIGNED NOT NULL,
  `node_sn`       VARCHAR(16) DEFAULT NULL COMMENT '网关自身告警为空',
  `type`          VARCHAR(24) NOT NULL COMMENT 'fall/gas/sos/no_motion/node_offline/device_offline/door_left/env',
  `level`         TINYINT UNSIGNED NOT NULL COMMENT '1 预警 / 2 报警',
  `ts`            DATETIME(3) NOT NULL,
  `ts_ms`         BIGINT UNSIGNED NOT NULL,
  `event_id`      VARCHAR(64) DEFAULT NULL COMMENT '幂等键：{sn}-{type}-{ts}',
  `status`        ENUM('pending','confirmed','resolved') NOT NULL DEFAULT 'pending',
  `recovered_at`  DATETIME(3) DEFAULT NULL COMMENT '条件消失时刻；非空即停推',
  `snapshot_json` JSON DEFAULT NULL,
  `push_count`    TINYINT UNSIGNED NOT NULL DEFAULT 0 COMMENT '上限 3',
  `last_push_at`  DATETIME(3) DEFAULT NULL,
  `next_push_at`  DATETIME(3) DEFAULT NULL COMMENT 'NULL = 不再推',
  `created_at`    DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_alerts_event` (`event_id`),
  KEY `idx_alerts_dev_status_ts` (`device_id`,`status`,`ts`),
  KEY `idx_alerts_repush` (`status`,`next_push_at`),
  KEY `idx_alerts_dev_type_ts` (`device_id`,`type`,`ts`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='告警（去重键 event_id）';

-- §7.8 alert_dispositions
CREATE TABLE IF NOT EXISTS `alert_dispositions` (
  `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `alert_id`   BIGINT UNSIGNED NOT NULL,
  `user_id`    BIGINT UNSIGNED NOT NULL,
  `action`     ENUM('contacted','checked','false_alarm','other') NOT NULL,
  `note`       VARCHAR(200) DEFAULT NULL,
  `created_at` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`id`),
  KEY `idx_disp_alert` (`alert_id`),
  KEY `idx_disp_user` (`user_id`,`created_at`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='告警处置记录（闭环）';

-- §7.9 activity_hourly
CREATE TABLE IF NOT EXISTS `activity_hourly` (
  `id`        BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id` BIGINT UNSIGNED NOT NULL,
  `date`      DATE NOT NULL COMMENT 'UTC+8 日历日',
  `hour`      TINYINT UNSIGNED NOT NULL COMMENT 'UTC+8 小时 0-23',
  `count`     INT UNSIGNED NOT NULL DEFAULT 0,
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_act_hourly` (`device_id`,`date`,`hour`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='活动分布（小时，UTC+8）';

-- §7.10 activity_daily
CREATE TABLE IF NOT EXISTS `activity_daily` (
  `id`             BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`      BIGINT UNSIGNED NOT NULL,
  `date`           DATE NOT NULL COMMENT 'UTC+8 日历日',
  `count`          INT UNSIGNED NOT NULL DEFAULT 0,
  `active_minutes` INT UNSIGNED NOT NULL DEFAULT 0 COMMENT '当天出现活动的分钟数（P1）',
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_act_daily` (`device_id`,`date`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='活动分布（日，UTC+8）';

-- §7.11 device_online
CREATE TABLE IF NOT EXISTS `device_online` (
  `id`                   BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `device_id`            BIGINT UNSIGNED NOT NULL,
  `online`               TINYINT(1) NOT NULL DEFAULT 0 COMMENT '180 s 规则（§8.7）',
  `last_heartbeat`       DATETIME(3) DEFAULT NULL,
  `last_heartbeat_ms`    BIGINT UNSIGNED DEFAULT NULL,
  `last_properties_json` JSON DEFAULT NULL COMMENT '最近属性快照（避免 /status 扫 readings）',
  `alarm_state`          TINYINT UNSIGNED NOT NULL DEFAULT 0,
  `updated_at`           DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_device_online` (`device_id`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='网关在线态与最近属性快照';

-- §7.12 push_tokens
CREATE TABLE IF NOT EXISTS `push_tokens` (
  `id`         BIGINT UNSIGNED NOT NULL AUTO_INCREMENT,
  `user_id`    BIGINT UNSIGNED NOT NULL,
  `token`      VARCHAR(255) NOT NULL,
  `platform`   ENUM('harmony','android') NOT NULL DEFAULT 'harmony',
  `created_at` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3),
  `updated_at` DATETIME(3) NOT NULL DEFAULT CURRENT_TIMESTAMP(3) ON UPDATE CURRENT_TIMESTAMP(3),
  PRIMARY KEY (`id`),
  UNIQUE KEY `uk_push_user_token` (`user_id`,`token`),
  KEY `idx_push_token` (`token`)
) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_0900_ai_ci COMMENT='Push Kit token';
