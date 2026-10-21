"""运行期配置：全部来自环境变量（契约 §6.1「密钥不进仓库」）。

设计取舍：
- 每个值都有 dev 兜底，保证 `docker compose up` 零配置可跑（验收要求）。
- APP_ENV=prod 时仍然用占位符密钥 → 直接拒绝启动，避免把演示口令带上生产。
"""
from __future__ import annotations

import os
from dataclasses import dataclass, field

# 占位符（.env.example 里的 dev 值）：prod 环境下一律拒绝启动
DEV_JWT_SECRET = "dev-only-change-me"
DEV_WEBHOOK_TOKEN = "dev-webhook-token-change-me"
PLACEHOLDER_SECRETS = {
    DEV_JWT_SECRET,
    DEV_WEBHOOK_TOKEN,
    "change-me",
    "changeme",
    "secret",
    "",
}


def _str(name: str, default: str = "") -> str:
    return os.getenv(name, default).strip()


def _int(name: str, default: int) -> int:
    raw = os.getenv(name)
    if raw is None or raw.strip() == "":
        return default
    try:
        return int(raw)
    except ValueError as exc:  # 配置错误要立刻可见，而不是跑起来后行为诡异
        raise RuntimeError(f"环境变量 {name} 必须是整数，实际为 {raw!r}") from exc


def _bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None or raw.strip() == "":
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def _list(name: str, default: list[str]) -> list[str]:
    raw = os.getenv(name)
    if raw is None or raw.strip() == "":
        return list(default)
    return [item.strip() for item in raw.split(",") if item.strip()]


@dataclass(frozen=True)
class Settings:
    app_env: str = "dev"
    version: str = "1.0.0"
    log_level: str = "INFO"

    # 数据库
    db_host: str = "127.0.0.1"
    db_port: int = 3306
    db_name: str = "eldercare"
    db_user: str = "eldercare"
    db_password: str = "hmprogram-dev-pass"
    db_auto_migrate: bool = True
    db_pool_size: int = 5

    # 鉴权
    jwt_secret: str = DEV_JWT_SECRET
    jwt_algorithm: str = "HS256"
    jwt_expire_days: int = 7
    webhook_token: str = DEV_WEBHOOK_TOKEN
    # 厂商侧出厂注册密钥（§6 补充接口 POST /api/device/activate，见 README「设备出厂注册」）：
    # 留空 = 接口整体 404（不暴露存在性）；它不是用户凭证，绝不发给 App，也不进 JWT。
    product_key: str = ""
    product_key_rate_limit: int = 5        # 出厂注册：同一 IP 每秒最多 5 次
    product_key_rate_window_s: int = 1

    # 业务常量（契约 §8）
    bind_code_ttl_min: int = 10          # §6.6 绑定码有效期 10 分钟
    gateway_offline_timeout_s: int = 180  # §8.7 网关离线判定
    clock_source: str = "server"          # §6.20 / CAL-09：未校时前必须是 server
    login_rate_limit: int = 100           # §9.1：同一 IP 5 分钟 >100 次登录尝试 → 429
    login_rate_window_s: int = 300
    idempotency_ttl_s: int = 300          # §6.1 Idempotency-Key 5 分钟
    pagination_default: int = 50
    pagination_max: int = 200

    # 外部系统
    iotda_dispatch_mode: str = "mock"     # mock | real
    iotda_endpoint: str = ""
    iotda_project_id: str = ""
    iotda_app_id: str = ""
    iotda_app_secret: str = ""
    iotda_iam_user: str = ""
    iotda_iam_password: str = ""
    iotda_iam_domain: str = ""
    push_mode: str = "mock"               # mock | real
    push_client_id: str = ""
    push_client_secret: str = ""

    scheduler_enabled: bool = True
    cors_origins: list[str] = field(default_factory=lambda: ["http://localhost", "http://127.0.0.1"])

    @property
    def is_prod(self) -> bool:
        return self.app_env.lower() in {"prod", "production"}

    @classmethod
    def from_env(cls) -> "Settings":
        s = cls(
            app_env=_str("APP_ENV", "dev"),
            version=_str("SERVER_VERSION", "1.0.0"),
            log_level=_str("LOG_LEVEL", "INFO").upper(),
            db_host=_str("DB_HOST", "127.0.0.1"),
            db_port=_int("DB_PORT", 3306),
            db_name=_str("DB_NAME", "eldercare"),
            db_user=_str("DB_USER", "eldercare"),
            db_password=_str("DB_PASSWORD", "hmprogram-dev-pass"),
            db_auto_migrate=_bool("DB_AUTO_MIGRATE", True),
            db_pool_size=_int("DB_POOL_SIZE", 5),
            jwt_secret=_str("JWT_SECRET", DEV_JWT_SECRET),
            jwt_expire_days=_int("JWT_EXPIRE_DAYS", 7),
            webhook_token=_str("WEBHOOK_TOKEN", DEV_WEBHOOK_TOKEN),
            product_key=_str("PRODUCT_KEY"),
            product_key_rate_limit=_int("PRODUCT_KEY_RATE_LIMIT", 5),
            product_key_rate_window_s=_int("PRODUCT_KEY_RATE_WINDOW_S", 1),
            gateway_offline_timeout_s=_int("GATEWAY_OFFLINE_TIMEOUT_S", 180),
            clock_source=_str("CLOCK_SOURCE", "server").lower(),
            iotda_dispatch_mode=_str("IOTDA_DISPATCH_MODE", "mock").lower(),
            iotda_endpoint=_str("IOTDA_ENDPOINT"),
            iotda_project_id=_str("IOTDA_PROJECT_ID"),
            iotda_app_id=_str("IOTDA_APP_ID"),
            iotda_app_secret=_str("IOTDA_APP_SECRET"),
            iotda_iam_user=_str("IOTDA_IAM_USER"),
            iotda_iam_password=_str("IOTDA_IAM_PASSWORD"),
            iotda_iam_domain=_str("IOTDA_IAM_DOMAIN"),
            push_mode=_str("PUSH_MODE", "mock").lower(),
            push_client_id=_str("PUSH_CLIENT_ID"),
            push_client_secret=_str("PUSH_CLIENT_SECRET"),
            scheduler_enabled=_bool("SCHEDULER_ENABLED", True),
            cors_origins=_list("CORS_ORIGINS", ["http://localhost", "http://127.0.0.1"]),
        )
        if s.is_prod and (s.jwt_secret in PLACEHOLDER_SECRETS or s.webhook_token in PLACEHOLDER_SECRETS):
            raise RuntimeError(
                "APP_ENV=prod 但 JWT_SECRET/WEBHOOK_TOKEN 仍是占位符：请通过环境变量注入真实随机值（不得进仓库）"
            )
        return s


settings = Settings.from_env()
