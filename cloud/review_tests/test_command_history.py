"""Command-history API regressions using isolated synthetic storage only."""
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sys

import pytest
from fastapi.testclient import TestClient


ROOT = Path(__file__).resolve().parents[1]
APP_PATH = Path(os.environ.get("RICE_COMMAND_HISTORY_TEST_APP", ROOT / "server" / "app.py"))


@pytest.fixture
def app_client(monkeypatch, tmp_path):
    salt = b"command-history-synthetic-salt"
    digest = hashlib.pbkdf2_hmac("sha256", b"synthetic-password", salt, 200_000)
    monkeypatch.setenv("RICE_ADMIN_PASSWORD_HASH", f"pbkdf2_sha256$200000${salt.hex()}${digest.hex()}")
    monkeypatch.setenv("RICE_DATA_DIR", str(tmp_path / "data"))
    monkeypatch.setenv("RICE_DB_PATH", str(tmp_path / "data" / "history.sqlite3"))
    monkeypatch.setenv("RICE_ANALYSIS_ENABLED", "false")
    monkeypatch.setenv("RICE_COOKIE_SECURE", "false")
    monkeypatch.setenv("RICE_PUBLIC_ORIGIN", "http://testserver")
    spec = importlib.util.spec_from_file_location("command_history_isolated_app", APP_PATH)
    module = importlib.util.module_from_spec(spec)
    monkeypatch.setitem(sys.modules, spec.name, module)
    spec.loader.exec_module(module)
    with TestClient(module.app) as client:
        yield module, client


def insert_command(module, command_id, *, station="plant-1", state="completed", created=100, updated=200, result=None):
    with module.db() as conn:
        conn.execute(
            "INSERT INTO commands(id,station_id,type,payload_json,state,created_at,updated_at,message,result_json) "
            "VALUES(?,?,'take_photo',?,?,?,?,?,?)",
            (command_id, station, json.dumps({"secret": "synthetic-payload-secret"}), state, created, updated,
             None if state == "cancelled" else "synthetic outcome", json.dumps(result) if result is not None else None),
        )


def authenticate(client):
    response = client.post("/api/login", json={"password": "synthetic-password"})
    assert response.status_code == 200


def snapshot(module):
    with module.db() as conn:
        return [tuple(row) for row in conn.execute("SELECT * FROM commands ORDER BY id")]


def test_history_is_bounded_station_scoped_terminal_and_read_only(app_client):
    module, client = app_client
    for command_id, state, created, updated in (
        ("old", "completed", 800, 50),
        ("failed", "failed", 100, 100),
        ("uncertain", "uncertain", 120, 120),
        ("cancelled", "cancelled", 140, 140),
        ("a-tied", "completed", 150, 200),
        ("z-tied", "completed", 150, 200),
    ):
        insert_command(module, command_id, state=state, created=created, updated=updated)
    insert_command(module, "pending", state="pending", updated=1000)
    insert_command(module, "unsupported", state="in_progress", updated=1001)
    insert_command(module, "other-station", station="plant-2", state="failed", updated=900)
    authenticate(client)
    before = snapshot(module)
    response = client.get("/api/state")
    assert response.status_code == 200
    stations = {station["station_id"]: station for station in response.json()["stations"]}
    history = stations["plant-1"]["recent_commands"]
    assert [command["id"] for command in history] == ["z-tied", "a-tied", "cancelled", "uncertain", "failed"]
    assert {command["state"] for command in history} == {"completed", "failed", "uncertain", "cancelled"}
    assert [command["id"] for command in stations["plant-2"]["recent_commands"]] == ["other-station"]
    assert stations["plant-3"]["recent_commands"] == []
    assert [command["id"] for command in stations["plant-1"]["pending_commands"]] == ["pending"]
    assert all(set(command) == {"id", "type", "state", "message", "result", "created_at", "updated_at"} for command in history)
    assert "synthetic-payload-secret" not in json.dumps(history)
    assert snapshot(module) == before


def test_history_preserves_result_and_converts_timestamps(app_client):
    module, client = app_client
    result = {"state": "completed", "message": "saved locally", "photo": {"date": "2026-10-22", "filename": "capture.jpg"}}
    insert_command(module, "photo-result", created=1_792_656_000, updated=1_792_656_123, result=result)
    insert_command(module, "legacy-cancelled", state="cancelled", created=10, updated=11)
    authenticate(client)
    history = client.get("/api/state").json()["stations"][0]["recent_commands"]
    assert history[0] == {
        "id": "photo-result", "type": "take_photo", "state": "completed", "message": "synthetic outcome",
        "result": result, "created_at": 1_792_656_000_000, "updated_at": 1_792_656_123_000,
    }
    assert history[1]["result"] is None
    assert history[1]["message"] is None
    assert history[1]["created_at"] == 10_000
    assert history[1]["updated_at"] == 11_000


def test_history_requires_admin_session(app_client):
    module, client = app_client
    insert_command(module, "private-result")
    before = snapshot(module)
    response = client.get("/api/state")
    assert response.status_code == 401
    assert "private-result" not in response.text
    assert snapshot(module) == before
