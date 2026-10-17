"""数据库访问层：SQLAlchemy Core + PyMySQL，SQL 全部手写（DDL 的唯一事实源是 db/schema.sql）。

为什么不用 ORM / 迁移框架：
- 契约 §7.13 规定 DDL 由 server/db/schema.sql 维护，且验收会断言表与唯一索引存在；
  ORM 模型会和这份 DDL 形成第二份「表结构事实」，反而容易漂移。
- 竞赛周期内表结构稳定，schema.sql 幂等（IF NOT EXISTS）+ 启动时执行，足够替代 alembic。
"""
from __future__ import annotations

import json
import logging
import pathlib
from contextlib import contextmanager
from typing import Any, Iterable, Iterator
from urllib.parse import quote_plus

import pymysql
from sqlalchemy import create_engine, event, text
from sqlalchemy.engine import Connection, Engine

from .config import settings

log = logging.getLogger("app.db")

SCHEMA_PATH = pathlib.Path(__file__).resolve().parent.parent / "db" / "schema.sql"

_engine: Engine | None = None


def _url(db_name: str | None = None) -> str:
    name = settings.db_name if db_name is None else db_name
    return (
        f"mysql+pymysql://{settings.db_user}:{quote_plus(settings.db_password)}"
        f"@{settings.db_host}:{settings.db_port}/{name}?charset=utf8mb4"
    )


def build_engine(db_name: str | None = None) -> Engine:
    eng = create_engine(
        _url(db_name),
        pool_pre_ping=True,
        pool_size=settings.db_pool_size,
        max_overflow=5,
        pool_recycle=1800,
        future=True,
        echo=False,
    )

    @event.listens_for(eng, "connect")
    def _init_session(dbapi_conn, _record):  # pragma: no cover - 由驱动回调
        # 契约 §0.2/§7.1：时间列一律 UTC，必须显式设置会话时区
        with dbapi_conn.cursor() as cur:
            cur.execute("SET time_zone = '+00:00'")

    return eng


def get_engine() -> Engine:
    global _engine
    if _engine is None:
        _engine = build_engine()
    return _engine


def reset_engine() -> None:
    """测试换库/换环境时用；生产不调用。"""
    global _engine
    if _engine is not None:
        _engine.dispose()
    _engine = None


def ensure_database(db_name: str | None = None) -> None:
    """建库（compose 的 MYSQL_DATABASE 已建好，这里兜底本机直连场景）。失败只告警不致命。"""
    name = db_name or settings.db_name
    try:
        conn = pymysql.connect(
            host=settings.db_host,
            port=settings.db_port,
            user=settings.db_user,
            password=settings.db_password,
            charset="utf8mb4",
            connect_timeout=5,
        )
    except Exception as exc:  # 连不上是启动阶段的事，交给 /api/health 暴露
        log.warning("建库前连接 MySQL 失败：%s", exc)
        return
    try:
        with conn.cursor() as cur:
            cur.execute(
                f"CREATE DATABASE IF NOT EXISTS `{name}` "
                "DEFAULT CHARACTER SET utf8mb4 COLLATE utf8mb4_0900_ai_ci"
            )
        conn.commit()
    except Exception as exc:
        log.warning("CREATE DATABASE %s 失败（可能权限不足，若库已存在可忽略）：%s", name, exc)
    finally:
        conn.close()


def schema_statements() -> list[str]:
    """把 schema.sql 拆成单条语句；去掉整行注释与空语句。"""
    raw = SCHEMA_PATH.read_text(encoding="utf-8")
    lines = [ln for ln in raw.splitlines() if not ln.strip().startswith("--")]
    statements = []
    for chunk in "\n".join(lines).split(";"):
        stmt = chunk.strip()
        if stmt:
            statements.append(stmt)
    return statements


def apply_schema() -> int:
    statements = schema_statements()
    with get_engine().begin() as conn:
        for stmt in statements:
            conn.execute(text(stmt))
    log.info("schema.sql 已执行：%d 条语句（幂等）", len(statements))
    return len(statements)


@contextmanager
def conn() -> Iterator[Connection]:
    """写事务：正常提交，异常回滚。"""
    with get_engine().begin() as connection:
        yield connection


def _exec(connection: Connection, sql: str, params: dict[str, Any] | Iterable[dict[str, Any]] | None):
    return connection.execute(text(sql), {} if params is None else params)


def fetch_all(sql: str, params: dict[str, Any] | None = None) -> list[dict[str, Any]]:
    with get_engine().connect() as connection:
        return [dict(row) for row in _exec(connection, sql, params).mappings().all()]


def fetch_one(sql: str, params: dict[str, Any] | None = None) -> dict[str, Any] | None:
    rows = fetch_all(sql, params)
    return rows[0] if rows else None


def fetch_scalar(sql: str, params: dict[str, Any] | None = None, default: Any = None) -> Any:
    row = fetch_one(sql, params)
    if not row:
        return default
    return next(iter(row.values()))


def execute(sql: str, params: dict[str, Any] | None = None) -> int:
    with conn() as connection:
        return _exec(connection, sql, params).rowcount


def execute_many(sql: str, params: list[dict[str, Any]]) -> int:
    if not params:
        return 0
    with conn() as connection:
        return _exec(connection, sql, params).rowcount


def insert(sql: str, params: dict[str, Any] | None = None) -> int:
    """返回自增主键。"""
    with conn() as connection:
        result = _exec(connection, sql, params)
        return int(result.lastrowid or 0)


def ping() -> bool:
    try:
        return fetch_scalar("SELECT 1") == 1
    except Exception as exc:
        log.warning("数据库 ping 失败：%s", exc)
        return False


def jdump(value: Any) -> str:
    """JSON 列参数一律显式序列化：让 SQLAlchemy 不参与类型猜测。"""
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"))


def jload(value: Any, default: Any = None) -> Any:
    if value is None:
        return default
    if isinstance(value, (dict, list)):
        return value
    try:
        return json.loads(value)
    except (TypeError, ValueError):
        return default
