"""Read-only implementation review; all mutable storage is pytest temporary storage.

The remaining repro tests characterize documented defects. Backup tests assert
the corrected rejection behavior. No network transport or worker is started.
"""
import asyncio
from contextlib import contextmanager
from concurrent.futures import ThreadPoolExecutor
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import sys
import tarfile
import threading

import httpx
import pytest


ROOT = Path(__file__).resolve().parents[1]


def password_hash(password):
    salt = b"review-synthetic-salt"
    digest = hashlib.pbkdf2_hmac("sha256", password.encode(), salt, 200_000)
    return f"pbkdf2_sha256$200000${salt.hex()}${digest.hex()}"


@pytest.fixture
def module(monkeypatch, tmp_path):
    monkeypatch.setenv("RICE_DATA_DIR", str(tmp_path / "data"))
    monkeypatch.setenv("RICE_DB_PATH", str(tmp_path / "data" / "rice-cloud.sqlite3"))
    monkeypatch.setenv("RICE_ADMIN_PASSWORD_HASH", password_hash("synthetic-old-password"))
    monkeypatch.setenv("RICE_ANALYSIS_ENABLED", "false")
    monkeypatch.setenv("RICE_COOKIE_SECURE", "false")
    monkeypatch.setenv("RICE_PUBLIC_ORIGIN", "http://review.test")
    spec = importlib.util.spec_from_file_location("server_review_isolated_app", ROOT / "server" / "app.py")
    result = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = result
    spec.loader.exec_module(result)
    result.initialize()
    with result.db() as conn:
        for station in ("plant-1", "plant-2"):
            conn.execute("INSERT INTO device_tokens(token_hash,station_id,created_at) VALUES(?,?,1)",
                         (result.token_hash("synthetic-token-" + station), station))
    return result


def backup_module():
    spec = importlib.util.spec_from_file_location("server_review_backup", ROOT / "deploy" / "backup.py")
    result = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(result)
    return result


def headers(payload, station="plant-1"):
    return {"Authorization": "Bearer synthetic-token-" + station,
            "X-Content-SHA256": hashlib.sha256(payload).hexdigest()}


def test_repro_marker_only_jpeg_is_accepted_and_preserved(module):
    async def scenario():
        payload = b"\xff\xd8\xffgarbage-without-jpeg-segments\xff\xd9"
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=module.app), base_url="http://review.test") as client:
            response = await client.put("/api/device/photos/2026-10-17/invalid.jpg", content=payload, headers=headers(payload))
        assert response.status_code == 200
        with module.db() as conn:
            row = conn.execute("SELECT path FROM photos").fetchone()
        assert (module.DATA_DIR / row["path"]).read_bytes() == payload
    asyncio.run(scenario())


def test_repro_rotating_admin_password_does_not_revoke_existing_session(module, monkeypatch):
    async def scenario():
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=module.app), base_url="http://review.test") as client:
            login = await client.post("/api/login", json={"password": "synthetic-old-password"})
            assert login.status_code == 200
            monkeypatch.setenv("RICE_ADMIN_PASSWORD_HASH", password_hash("synthetic-new-password"))
            module.startup()  # Same behavior as service restart with the changed hash.
            state = await client.get("/api/state")
            assert state.status_code == 200
            command = await client.post("/api/stations/plant-1/commands", json={"type": "take_photo"},
                                        headers={"Origin": "http://review.test", "X-CSRF-Token": login.json()["csrf_token"]})
            assert command.status_code == 200
            assert (await client.post("/api/login", json={"password": "synthetic-old-password"})).status_code == 401
    asyncio.run(scenario())


def test_backup_rejects_photo_corruption_against_database_checksum(module, tmp_path):
    relative = "photos/plant-1/2026-10-17/photo.jpg"
    photo = module.DATA_DIR / relative
    photo.parent.mkdir(parents=True)
    original = b"\xff\xd8\xfforiginal-synthetic-photo\xff\xd9"
    photo.write_bytes(b"CORRUPTED-PHOTO")
    checksum = hashlib.sha256(original).hexdigest()
    with module.db() as conn:
        conn.execute("INSERT INTO photos(station_id,date,filename,checksum,path,uploaded_at) VALUES('plant-1','2026-10-17','photo.jpg',?,?,1)",
                     (checksum, relative))
    backup = backup_module()
    with pytest.raises(RuntimeError, match="checksum"):
        backup.create_backup(module.DATA_DIR, tmp_path / "backups")


def test_restore_verification_rejects_missing_referenced_photo(module, tmp_path):
    relative = "photos/plant-1/2026-10-17/missing.jpg"
    with module.db() as conn:
        conn.execute("INSERT INTO photos(station_id,date,filename,checksum,path,uploaded_at) VALUES('plant-1','2026-10-17','missing.jpg',?,?,1)",
                     ("0" * 64, relative))
    # A valid SQLite database plus empty manifest must not pass restore verification.
    manifest = tmp_path / "manifest.json"
    manifest.write_text("{}")
    archive = tmp_path / "incomplete.tar.gz"
    with tarfile.open(archive, "w:gz") as packed:
        packed.add(module.DB_PATH, arcname="rice-cloud.sqlite3")
        packed.add(manifest, arcname="manifest.json")
    with pytest.raises(RuntimeError, match="manifest"):
        backup_module().verify_backup(archive)


def test_repro_concurrent_commands_exceed_eight_pending_limit(module, monkeypatch):
    for _ in range(7):
        module.create_command("plant-1", module.CommandBody(type="take_photo"), None)
    real_db = module.db
    barrier = threading.Barrier(2)

    class CountCursor:
        def __init__(self, cursor):
            self.cursor = cursor

        def fetchone(self):
            value = self.cursor.fetchone()
            barrier.wait(timeout=10)
            return value

    class Connection:
        def __init__(self, connection):
            self.connection = connection

        def execute(self, sql, *args):
            cursor = self.connection.execute(sql, *args)
            if sql.startswith("SELECT COUNT(*) FROM commands"):
                return CountCursor(cursor)
            return cursor

    @contextmanager
    def synchronized_db():
        with real_db() as conn:
            yield Connection(conn)

    monkeypatch.setattr(module, "db", synchronized_db)
    with ThreadPoolExecutor(max_workers=2) as pool:
        results = list(pool.map(lambda _: module.create_command("plant-1", module.CommandBody(type="take_photo"), None), range(2)))
    assert len(results) == 2
    with real_db() as conn:
        assert conn.execute("SELECT COUNT(*) FROM commands WHERE state='pending'").fetchone()[0] == 9


def test_isolation_csrf_revocation_traversal_and_upload_recovery(module):
    async def scenario():
        payload = b"\xff\xd8\xffsynthetic-complete-marker-image\xff\xd9"
        url = "/api/device/photos/2026-10-17/good.jpg"
        async with httpx.AsyncClient(transport=httpx.ASGITransport(app=module.app), base_url="http://review.test") as client:
            assert (await client.get("/api/state")).status_code == 401
            assert (await client.post("/api/device/sync", json={"station_id": "plant-2"}, headers=headers(payload))).status_code == 403
            login = await client.post("/api/login", json={"password": "synthetic-old-password"})
            csrf = login.json()["csrf_token"]
            assert (await client.post("/api/stations/plant-1/commands", json={"type": "take_photo"})).status_code == 403
            assert (await client.post("/api/stations/plant-1/commands", json={"type": "take_photo"}, headers={"Origin": "http://foreign.test", "X-CSRF-Token": csrf})).status_code == 403
            first = await client.put(url, content=payload, headers=headers(payload))
            assert first.status_code == 200
            with module.db() as conn:
                row = conn.execute("SELECT path FROM photos").fetchone()
            photo = module.DATA_DIR / row["path"]
            photo.write_bytes(b"corrupted synthetic file")
            recovery = await client.put(url, content=payload, headers=headers(payload))
            assert recovery.status_code == 200 and recovery.json()["duplicate"] is True
            assert photo.read_bytes() == payload
            for name in ("%2e%2e%2foutside.jpg", "%5c..%5coutside.jpg"):
                assert (await client.put("/api/device/photos/2026-10-17/" + name, content=payload, headers=headers(payload))).status_code in {400, 404, 405}
            with module.db() as conn:
                conn.execute("UPDATE device_tokens SET revoked_at=1 WHERE station_id='plant-1'")
            assert (await client.put(url, content=payload, headers=headers(payload))).status_code == 401
            assert (await client.post("/api/logout", headers={"Origin": "http://review.test", "X-CSRF-Token": csrf})).status_code == 200
            assert (await client.get("/api/state")).status_code == 401
    asyncio.run(scenario())
