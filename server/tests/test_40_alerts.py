"""§6.13–§6.15 告警：列表 / 详情 / 处置 + §8.8 去重与升级 + §7.7 重推。

走真实链路（模拟 IoTDA 规则引擎 webhook），不直接调服务层——这样契约里
「webhook body → alerts 表 → REST 响应」整条链路都在测试覆盖内。
"""
from __future__ import annotations

from app import db
from app.services.scheduler import tick_once
from app.timeutil import dt_to_ms, ms_to_dt, now_ms

from conftest import GATEWAY_SN, create_user, login

WEBHOOK = "/api/iotda/webhook"


def _auth(token: str) -> dict:
    return {"Authorization": f"Bearer {token}"}


def push_alert(client, device, alert_type="fall", level=2, node_sn="M2", ts=None, event_id=None, snapshot=None):
    """按 §4.6 事件报文构造 webhook body（字段与平台实际下发一致）。"""
    from app.config import settings

    ts = now_ms() if ts is None else ts
    event_id = event_id or f"{GATEWAY_SN}-{alert_type}-{ts}"
    return client.post(
        f"{WEBHOOK}?token={settings.webhook_token}",
        json={
            "resource": "device.event",
            "event": "alert",
            "event_time": "20261017T120000Z",
            "notify_data": {
                "header": {"device_id": device["sn"], "node_id": GATEWAY_SN},
                "body": {
                    "services": [
                        {
                            "service_id": "gateway",
                            "event_type": "alert",
                            "event_time": "20261017T120000Z",
                            "paras": {
                                "type": alert_type,
                                "level": level,
                                "node_sn": node_sn,
                                "ts": ts,
                                "event_id": event_id,
                                "snapshot": snapshot or {"fall": 1, "presence": 1},
                            },
                        }
                    ]
                },
            },
        },
    )


# --------------------------------------------------------------------------- §6.13 列表

def test_alert_list_shape_and_summary(client, device):
    assert push_alert(client, device).status_code == 200
    resp = client.get("/api/alerts", headers=_auth(device["token"]), params={"device_id": device["id"]})
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert data["total"] == 1
    item = data["items"][0]
    # §0.4：未恢复/未推送时 recovered_at/last_push_at/next_push_at **省略键**，不给 null
    assert {"id", "device_id", "node_sn", "type", "level", "ts", "status", "push_count",
            "summary"} <= set(item)
    assert set(item) <= {"id", "device_id", "node_sn", "type", "level", "ts", "status",
                         "recovered_at", "push_count", "last_push_at", "next_push_at", "summary"}
    assert item["status"] == "pending"
    assert item["level"] == 2 and item["node_sn"] == "M2"
    # §6.13 文案表（逐字）
    assert item["summary"] == "卧室检测到跌倒"
    assert item["push_count"] == 1                            # §7.7 第一档 0 s：新建即投递
    assert item["last_push_at"] is not None
    assert item["next_push_at"] - item["ts"] == 60_000             # 下一档 = 告警时刻 + 60 s（§7.7）


def test_alert_summaries_match_contract_wording(client, device):
    cases = [
        ("fall", 2, "M2", "卧室检测到跌倒"),
        ("gas", 1, "M1", "厨房燃气浓度偏高"),
        ("gas", 2, "M1", "厨房燃气泄漏报警"),
        ("sos", 2, "M4", "收到紧急求助"),
        ("no_motion", 1, "M4", "30 分钟未检测到活动"),
        ("node_offline", 1, "M1", "节点 M1 已离线"),
        ("device_offline", 1, GATEWAY_SN, "网关已离线"),
        ("door_left", 1, "M4", "门已打开超过 3 分钟"),
        ("env", 1, "M1", "室内温湿度异常"),
    ]
    from app.timeutil import now_ms

    base = now_ms() - 3_600_000
    for index, (alert_type, level, node_sn, expected) in enumerate(cases):
        ts = base + index * 120_000
        resp = push_alert(client, device, alert_type=alert_type, level=level, node_sn=node_sn, ts=ts,
                          event_id=f"{GATEWAY_SN}-{alert_type}-{ts}")
        assert resp.status_code == 200, resp.text
    listed = client.get("/api/alerts", headers=_auth(device["token"]),
                        params={"device_id": device["id"], "limit": 50}).json()["data"]["items"]
    by_type = {}
    for item in listed:
        by_type.setdefault(item["type"], []).append(item)
    for alert_type, level, node_sn, expected in cases:
        hit = [i for i in by_type[alert_type] if i["level"] == level]
        assert hit, alert_type
        assert hit[0]["summary"] == expected


def test_alert_list_filters_status_and_type(client, device):
    from app.timeutil import now_ms

    push_alert(client, device)
    push_alert(client, device, alert_type="gas", level=1, node_sn="M1", ts=now_ms() - 600_000)
    only_fall = client.get("/api/alerts", headers=_auth(device["token"]),
                           params={"device_id": device["id"], "type": "fall"}).json()["data"]
    assert only_fall["total"] == 1 and only_fall["items"][0]["type"] == "fall"
    pending = client.get("/api/alerts", headers=_auth(device["token"]),
                         params={"device_id": device["id"], "status": "pending"}).json()["data"]
    assert pending["total"] == 2
    resolved = client.get("/api/alerts", headers=_auth(device["token"]),
                          params={"device_id": device["id"], "status": "resolved"}).json()["data"]
    assert resolved["total"] == 0 and resolved["items"] == []
    # §6.13：limit 越界要按 1002 拒绝（200 是上限）
    bad = client.get("/api/alerts", headers=_auth(device["token"]),
                     params={"device_id": device["id"], "limit": 500})
    assert bad.status_code == 400 and bad.json()["code"] == 1002
    # §6.13：status 未知取值契约明文回 1001（枚举非法，不是 1002 的取值越界）
    bogus = client.get("/api/alerts", headers=_auth(device["token"]),
                       params={"device_id": device["id"], "status": "bogus"})
    assert bogus.status_code == 400 and bogus.json()["code"] == 1001, bogus.text
    assert bogus.json()["data"]["allowed"] == ["pending", "confirmed", "resolved", "all"]


def test_alert_list_requires_device_membership(client, device):
    stranger = create_user()
    token = login(client, stranger["username"])
    resp = client.get("/api/alerts", headers=_auth(token), params={"device_id": device["id"]})
    assert resp.status_code == 403 and resp.json()["code"] == 1104


# --------------------------------------------------------------------------- §6.14 详情

def test_alert_detail_confirms_and_freezes_push(client, device):
    push_alert(client, device)
    alert_id = client.get("/api/alerts", headers=_auth(device["token"]),
                          params={"device_id": device["id"]}).json()["data"]["items"][0]["id"]
    resp = client.get(f"/api/alerts/{alert_id}", headers=_auth(device["token"]))
    assert resp.status_code == 200
    data = resp.json()["data"]
    # PM 裁定（契约 v1.2）：详情 data **平铺**，不能出现 alert 子对象
    assert "alert" not in data
    assert {"id", "device_id", "type", "level", "status", "summary", "event_id",
            "dispositions", "readings_before", "readings_after"} <= set(data)
    assert len(data["readings_before"]) <= 5 and len(data["readings_after"]) <= 5
    assert data["dispositions"] == []
    # §6.15：打开详情 = 已读 → confirmed + 停推
    assert data["status"] == "confirmed"
    assert "next_push_at" not in data                       # 停推 = 键省略（§0.4）
    row = db.fetch_one("SELECT status FROM alerts WHERE id = :i", {"i": alert_id})
    assert row["status"] == "confirmed"


def test_alert_detail_unknown_1300(client, user):
    resp = client.get("/api/alerts/424242", headers=_auth(user["token"]))
    assert resp.status_code == 404 and resp.json()["code"] == 1300


def test_alert_detail_forbidden_for_stranger_1104(client, device):
    push_alert(client, device)
    alert_id = client.get("/api/alerts", headers=_auth(device["token"]),
                          params={"device_id": device["id"]}).json()["data"]["items"][0]["id"]
    stranger = create_user()
    token = login(client, stranger["username"])
    resp = client.get(f"/api/alerts/{alert_id}", headers=_auth(token))
    assert resp.status_code == 403 and resp.json()["code"] == 1104


# --------------------------------------------------------------------------- §6.15 处置

def test_disposition_closes_alert(client, device):
    push_alert(client, device)
    alert_id = client.get("/api/alerts", headers=_auth(device["token"]),
                          params={"device_id": device["id"]}).json()["data"]["items"][0]["id"]
    resp = client.post(f"/api/alerts/{alert_id}/disposition", headers=_auth(device["token"]),
                       json={"action": "checked", "note": "已上门确认，老人安好"})
    assert resp.status_code == 200
    data = resp.json()["data"]
    assert set(data) == {"alert_id", "status", "action", "disposition_id", "ts"}
    assert data["status"] == "resolved" and data["action"] == "checked"
    row = db.fetch_one("SELECT status, next_push_at FROM alerts WHERE id = :i", {"i": alert_id})
    assert row["status"] == "resolved" and row["next_push_at"] is None
    detail = client.get(f"/api/alerts/{alert_id}", headers=_auth(device["token"])).json()["data"]
    assert len(detail["dispositions"]) == 1
    assert detail["dispositions"][0]["action"] == "checked"
    assert detail["dispositions"][0]["note"] == "已上门确认，老人安好"
    # §6.14：处置记录的时间键是 `ts`（不是 `at`），App 详情页按 ts 显示处置时间
    assert set(detail["dispositions"][0]) == {"id", "user_id", "username", "action", "note", "ts"}
    assert isinstance(detail["dispositions"][0]["ts"], int) and detail["dispositions"][0]["ts"] > 0


def test_disposition_twice_is_1301(client, device):
    push_alert(client, device)
    alert_id = client.get("/api/alerts", headers=_auth(device["token"]),
                          params={"device_id": device["id"]}).json()["data"]["items"][0]["id"]
    assert client.post(f"/api/alerts/{alert_id}/disposition", headers=_auth(device["token"]),
                       json={"action": "contacted"}).status_code == 200
    again = client.post(f"/api/alerts/{alert_id}/disposition", headers=_auth(device["token"]),
                        json={"action": "checked"})
    assert again.status_code == 409 and again.json()["code"] == 1301


def test_disposition_bad_action_1302(client, device):
    push_alert(client, device)
    alert_id = client.get("/api/alerts", headers=_auth(device["token"]),
                          params={"device_id": device["id"]}).json()["data"]["items"][0]["id"]
    resp = client.post(f"/api/alerts/{alert_id}/disposition", headers=_auth(device["token"]),
                       json={"action": "ignored"})
    assert resp.status_code == 400 and resp.json()["code"] == 1302


# --------------------------------------------------------------------------- §8.8 去重/升级 + §7.7 重推

def test_duplicate_event_id_is_idempotent(client, device):
    from app.timeutil import now_ms

    ts = now_ms()
    first = push_alert(client, device, ts=ts, event_id=f"{GATEWAY_SN}-fall-{ts}")
    second = push_alert(client, device, ts=ts, event_id=f"{GATEWAY_SN}-fall-{ts}")
    assert first.status_code == 200 and second.status_code == 200
    count = db.fetch_scalar("SELECT COUNT(*) FROM alerts WHERE device_id = :d", {"d": device["id"]}, 0)
    assert int(count) == 1                      # event_id 唯一键挡重报（§4.4）
    assert db.fetch_scalar("SELECT push_count FROM alerts WHERE device_id = :d", {"d": device["id"]}, 0) == 1


def test_suppress_window_merges_repeats_without_resetting_push(client, device):
    """§8.8：窗口内重复不新建、不动 push_count/next_push_at，只更新快照。"""
    from app.timeutil import now_ms

    base = now_ms()
    push_alert(client, device, ts=base, snapshot={"fall": 1})
    push_alert(client, device, ts=base + 5_000, snapshot={"fall": 1, "presence_count": 2})
    rows = db.fetch_all("SELECT push_count, snapshot_json FROM alerts WHERE device_id = :d", {"d": device["id"]})
    assert len(rows) == 1
    assert rows[0]["push_count"] == 1
    assert db.jload(rows[0]["snapshot_json"])["presence_count"] == 2


def test_level_escalation_creates_new_alert_and_recovers_old(client, device):
    """§8.8：窗口内 1→2 升级 → 新建 level=2，旧条置 recovered_at。"""
    from app.timeutil import now_ms

    base = now_ms()
    push_alert(client, device, alert_type="gas", level=1, node_sn="M1", ts=base)
    push_alert(client, device, alert_type="gas", level=2, node_sn="M1", ts=base + 10_000)
    rows = db.fetch_all(
        "SELECT id, level, status, recovered_at FROM alerts WHERE device_id = :d ORDER BY level",
        {"d": device["id"]},
    )
    assert [int(r["level"]) for r in rows] == [1, 2]
    assert rows[0]["recovered_at"] is not None
    assert rows[1]["recovered_at"] is None and rows[1]["status"] == "pending"


def test_recover_event_marks_recovered_without_closing(client, device):
    """§8.4：设备侧恢复只写 recovered_at 并停推，status 仍 pending，闭环要人工处置。"""
    from app.config import settings
    from app.timeutil import now_ms

    push_alert(client, device)
    alert_id = client.get("/api/alerts", headers=_auth(device["token"]),
                          params={"device_id": device["id"]}).json()["data"]["items"][0]["id"]
    ts = now_ms()
    resp = client.post(f"{WEBHOOK}?token={settings.webhook_token}", json={
        "resource": "device.event", "event": "recover",
        "notify_data": {"header": {"device_id": device["sn"]},
                        "body": {"services": [{"service_id": "gateway", "event_type": "recover",
                                               "paras": {"type": "fall", "node_sn": "M2", "ts": ts,
                                                         "event_id": f"{GATEWAY_SN}-recover-{ts}"}}]}},
    })
    assert resp.status_code == 200
    row = db.fetch_one("SELECT status, recovered_at, next_push_at FROM alerts WHERE id = :i", {"i": alert_id})
    assert row["status"] == "pending"
    assert row["recovered_at"] is not None and row["next_push_at"] is None


def test_repush_schedule_60_then_300_then_stop(client, device):
    """§7.7：节奏 [0,60,300] —— 首推同步发生，60 s/300 s 两档由调度器接手，第 3 次后永不再推。"""
    push_alert(client, device)
    alert_id = client.get("/api/alerts", headers=_auth(device["token"]),
                          params={"device_id": device["id"]}).json()["data"]["items"][0]["id"]

    def _row():
        return db.fetch_one("SELECT push_count, next_push_at, status FROM alerts WHERE id = :i", {"i": alert_id})

    assert int(_row()["push_count"]) == 1                  # 首推已发生，next = ts+60
    seen = []
    for _ in range(3):
        row = _row()
        if row["next_push_at"] is not None:
            # 把下一档拨到「现在」模拟时间流逝；next_push_at=NULL 后不再拨，验证它真的停推
            db.execute("UPDATE alerts SET next_push_at = :n WHERE id = :i",
                       {"n": ms_to_dt(now_ms()), "i": alert_id})
        tick_once()
        row = _row()
        seen.append((int(row["push_count"]), dt_to_ms(row["next_push_at"])))
    assert [count for count, _ in seen] == [2, 3, 3]
    assert seen[1][1] is None and seen[2][1] is None

    # confirmed / resolved / 已恢复的告警不参与重推（§7.7 扫描条件）
    push_alert(client, device, alert_type="gas", level=1, node_sn="M1")
    gas_id = db.fetch_scalar("SELECT id FROM alerts WHERE type = 'gas' AND device_id = :d",
                             {"d": device["id"]}, 0)
    db.execute("UPDATE alerts SET status = 'confirmed', next_push_at = :n WHERE id = :i",
               {"n": ms_to_dt(now_ms()), "i": gas_id})
    before = db.fetch_scalar("SELECT push_count FROM alerts WHERE id = :i", {"i": gas_id}, 0)
    tick_once()
    assert db.fetch_scalar("SELECT push_count FROM alerts WHERE id = :i", {"i": gas_id}, 0) == before

    db.execute("UPDATE alerts SET status = 'pending', recovered_at = :r, next_push_at = :n WHERE id = :i",
               {"r": ms_to_dt(now_ms()), "n": ms_to_dt(now_ms()), "i": gas_id})
    tick_once()
    assert db.fetch_scalar("SELECT push_count FROM alerts WHERE id = :i", {"i": gas_id}, 0) == before


def test_device_push_schedule_override(client, device):
    """§6.9 `alert_repush_s` 是设备级设置：改小后重推节奏必须跟着变（不是写死的常量）。"""
    put = client.put(f"/api/devices/{device['id']}/settings", headers=_auth(device["token"]),
                     json={"items": {"alert_repush_s": [0, 10, 20]}})
    assert put.status_code == 200, put.text
    push_alert(client, device)
    alert_id = db.fetch_scalar("SELECT id FROM alerts WHERE device_id = :d", {"d": device["id"]}, 0)
    for expected in (2, 3, 3):
        row = db.fetch_one("SELECT push_count, next_push_at FROM alerts WHERE id = :i", {"i": alert_id})
        if row["next_push_at"] is not None:
            db.execute("UPDATE alerts SET next_push_at = :n WHERE id = :i",
                       {"n": ms_to_dt(now_ms()), "i": alert_id})
        tick_once()
        row = db.fetch_one("SELECT push_count, next_push_at FROM alerts WHERE id = :i", {"i": alert_id})
        assert int(row["push_count"]) == expected
    assert row["next_push_at"] is None                     # 三档走完 → 停推
