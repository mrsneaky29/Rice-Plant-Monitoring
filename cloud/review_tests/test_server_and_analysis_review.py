"""Synthetic regression tests for server APIs, analysis worker, and command handling."""
import hashlib
import importlib
import json
import os
import sqlite3
import sys
from pathlib import Path
import pytest
from fastapi.testclient import TestClient

SERVER_DIR = Path(__file__).resolve().parent.parent / "server"
if str(SERVER_DIR) not in sys.path:
    sys.path.insert(0, str(SERVER_DIR))


def make_hash(password="correct horse battery staple"):
    salt = b"0123456789abcdef"
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 200_000)
    return f"pbkdf2_sha256$200000${salt.hex()}${digest.hex()}"


def setup_client(monkeypatch, tmp_path):
    monkeypatch.setenv("RICE_DATA_DIR", str(tmp_path / "data"))
    monkeypatch.setenv("RICE_DB_PATH", str(tmp_path / "data" / "test.sqlite3"))
    monkeypatch.setenv("RICE_ADMIN_PASSWORD_HASH", make_hash())
    monkeypatch.setenv("RICE_COOKIE_SECURE", "false")
    monkeypatch.setenv("RICE_ANALYSIS_ENABLED", "false")
    monkeypatch.delenv("RICE_PUBLIC_ORIGIN", raising=False)
    import app as module
    module = importlib.reload(module)
    return module, TestClient(module.app)


def device_token(module, station="plant-1"):
    raw = "rcd_test_" + station
    with module.db() as conn:
        conn.execute(
            "INSERT INTO device_tokens(token_hash,station_id,created_at) VALUES(?,?,1)",
            (module.token_hash(raw), station)
        )
    return raw


def test_reproduce_test_api_sd_archive_upload_failure_without_context(monkeypatch, tmp_path):
    """Reproduces the failure in server/tests/test_api.py::test_sd_archive_upload_never_triggers_paid_backfill.

    In test_api.py:329-343, setup_client() returns a TestClient, but it is not entered
    with 'with client:' or client.__enter__().
    Because lifespan startup initializes the database tables (init_db/initialize),
    calling device_token() directly raises sqlite3.OperationalError: no such table: device_tokens.
    Then, the finally block attempts client.__exit__(None, None, None), which raises
    AttributeError: 'TestClient' object has no attribute 'exit_stack'.
    """
    module, client = setup_client(monkeypatch, tmp_path)
    module.ANALYSIS_ENABLED = True
    try:
        with pytest.raises(sqlite3.OperationalError, match="no such table: device_tokens"):
            device_token(module)
    finally:
        module.ANALYSIS_ENABLED = False
        with pytest.raises(AttributeError, match="exit_stack"):
            client.__exit__(None, None, None)


def test_sd_archive_upload_behavior_with_proper_context(monkeypatch, tmp_path):
    """Verifies that with proper TestClient lifespan context:
    1. X-Rice-Archive-Import: true skips analysis job queueing.
    2. Upload without the header queues exactly one analysis job when enabled.
    """
    module, client = setup_client(monkeypatch, tmp_path)
    with client:
        module.ANALYSIS_ENABLED = True
        try:
            token = device_token(module)
            photo1 = b"\xff\xd8\xff" + b"archived-image-1" + b"\xff\xd9"
            headers_archive = {
                "Authorization": f"Bearer {token}",
                "X-Content-SHA256": hashlib.sha256(photo1).hexdigest(),
                "X-Rice-Archive-Import": "true"
            }
            res1 = client.put("/api/device/photos/2026-09-14/archive.jpg", content=photo1, headers=headers_archive)
            assert res1.status_code == 200

            with module.db() as conn:
                assert conn.execute("SELECT COUNT(*) FROM photos").fetchone()[0] == 1
                # Should not have queued any analysis job
                assert conn.execute("SELECT COUNT(*) FROM analysis_jobs").fetchone()[0] == 0

            # Normal upload without archive import header
            photo2 = b"\xff\xd8\xff" + b"new-image-2" + b"\xff\xd9"
            headers_new = {
                "Authorization": f"Bearer {token}",
                "X-Content-SHA256": hashlib.sha256(photo2).hexdigest(),
            }
            res2 = client.put("/api/device/photos/2026-10-17/fresh.jpg", content=photo2, headers=headers_new)
            assert res2.status_code == 200

            with module.db() as conn:
                assert conn.execute("SELECT COUNT(*) FROM photos").fetchone()[0] == 2
                # Exactly one job queued for the fresh photo
                assert conn.execute("SELECT COUNT(*) FROM analysis_jobs").fetchone()[0] == 1
                job = conn.execute("SELECT state,model FROM analysis_jobs WHERE photo_id=?", (res2.json()["photo_id"],)).fetchone()
                assert job["state"] == "queued"
                assert job["model"] == "gpt-6.1-sol"
        finally:
            module.ANALYSIS_ENABLED = False


def test_command_cancellation_race_returns_409(monkeypatch, tmp_path):
    """Tests the command cancellation race condition:
    When an admin cancels a command while the device is executing it,
    the device's subsequent result report returns 409 Conflict.
    """
    module, client = setup_client(monkeypatch, tmp_path)
    with client:
        # 1. Admin logs in and queues a command
        login_res = client.post("/api/login", json={"password": "correct horse battery staple"})
        csrf = login_res.json()["csrf_token"]
        headers_admin = {"Origin": "http://testserver", "X-CSRF-Token": csrf}

        cmd_res = client.post(
            "/api/stations/plant-1/commands",
            json={"type": "take_photo", "payload": {}},
            headers=headers_admin
        )
        assert cmd_res.status_code == 200
        cmd_id = cmd_res.json()["id"]

        # 2. Device sync retrieves the command
        token = device_token(module, "plant-1")
        sync_res = client.post(
            "/api/device/sync",
            json={"station_id": "plant-1", "sleep_enabled": False, "awake": True, "status": {}},
            headers={"Authorization": f"Bearer {token}"}
        )
        assert sync_res.status_code == 200
        cmds = sync_res.json()["commands"]
        assert len(cmds) == 1
        assert cmds[0]["id"] == cmd_id

        # 3. Admin cancels command while camera is capturing
        cancel_res = client.post(
            f"/api/stations/plant-1/commands/{cmd_id}/cancel",
            headers=headers_admin
        )
        assert cancel_res.status_code == 200

        # 4. Device finishes capture and attempts to post completed result
        result_payload = {
            "state": "completed",
            "message": "captured successfully",
            "photo": {"date": "2026-10-17", "filename": "photo.jpg"}
        }
        post_res = client.post(
            f"/api/device/commands/{cmd_id}/result",
            json=result_payload,
            headers={"Authorization": f"Bearer {token}"}
        )
        # Server rejects device report with 409 Conflict because state is no longer pending
        assert post_res.status_code == 409
        assert "no longer pending" in post_res.json()["detail"]
