import hashlib
import importlib
import json
import sqlite3
import sys
from pathlib import Path

from fastapi.testclient import TestClient

SERVER_DIR = Path(__file__).resolve().parents[1]
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
        conn.execute("INSERT INTO device_tokens(token_hash,station_id,created_at) VALUES(?,?,1)", (module.token_hash(raw), station))
    return raw


def login(client):
    response = client.post("/api/login", json={"password": "correct horse battery staple"})
    assert response.status_code == 200
    assert "httponly" in response.headers["set-cookie"].lower()
    return response.json()["csrf_token"]


def test_login_csrf_and_rate_limit(monkeypatch, tmp_path):
    module, client = setup_client(monkeypatch, tmp_path)
    with client:
        assert client.get("/api/state").status_code == 401
        for _ in range(8):
            assert client.post("/api/login", json={"password": "wrong"}).status_code == 401
        assert client.post("/api/login", json={"password": "correct horse battery staple"}).status_code == 429
        with module.db() as conn:
            conn.execute("DELETE FROM login_attempts")
        csrf = login(client)
        assert client.get("/api/state").status_code == 200
        assert client.post("/api/stations/plant-1/commands", json={"type": "take_photo"}).status_code == 403
        response = client.post("/api/stations/plant-1/commands", json={"type": "take_photo", "payload": {}}, headers={"Origin": "http://testserver", "X-CSRF-Token": csrf})
        assert response.status_code == 200
        assert client.post("/api/logout", headers={"Origin": "http://testserver", "X-CSRF-Token": csrf}).status_code == 200
        assert client.get("/api/state").status_code == 401


def test_station_binding_command_repeat_uncertain_ack_and_settings_validation(monkeypatch, tmp_path):
    module, client = setup_client(monkeypatch, tmp_path)
    with client:
        token1 = device_token(module, "plant-1")
        token2 = device_token(module, "plant-2")
        csrf = login(client)
        headers = {"Origin": "http://testserver", "X-CSRF-Token": csrf}
        invalid = client.post("/api/stations/plant-1/commands", json={"type": "set_settings", "payload": {"interval_seconds": 999999}}, headers=headers)
        assert invalid.status_code == 422
        payload = {"sleep_enabled": True, "timer_enabled": True, "interval_seconds": 21600, "window_enabled": True, "window_start": "06:30", "window_end": "18:30"}
        command = client.post("/api/stations/plant-1/commands", json={"type": "set_settings", "payload": payload}, headers=headers).json()
        auth = {"Authorization": f"Bearer {token1}"}
        assert client.post("/api/device/sync", json={"station_id": "plant-2"}, headers=auth).status_code == 403
        sync = client.post("/api/device/sync", json={"station_id": "plant-1", "status": {"interval_seconds": 21600}}, headers=auth)
        assert sync.json()["commands"][0]["id"] == command["id"]
        assert client.get("/api/device/commands", headers=auth).json()["commands"][0]["id"] == command["id"]
        assert client.post(f"/api/device/commands/{command['id']}/result", json={"state": "uncertain"}, headers=auth).json()["ok"]
        assert client.get("/api/device/commands", headers=auth).json()["commands"] == []
        assert client.get("/api/device/commands", headers={"Authorization": f"Bearer {token2}"}).json()["commands"] == []
        assert client.post(f"/api/device/commands/{command['id']}/result", json={"state": "completed"}, headers={"Authorization": f"Bearer {token2}"}).status_code == 404
        queued = [client.post("/api/stations/plant-1/commands", json={"type": "take_photo", "payload": {}}, headers=headers).json() for _ in range(8)]
        assert client.post("/api/stations/plant-1/commands", json={"type": "take_photo", "payload": {}}, headers=headers).status_code == 409
        cancel_url = f"/api/stations/plant-1/commands/{queued[0]['id']}/cancel"
        assert client.post(cancel_url, headers=headers).status_code == 200
        assert len(client.get("/api/device/commands", headers=auth).json()["commands"]) == 7


def test_photo_validation_idempotency_conflict_and_protected_access(monkeypatch, tmp_path):
    module, client = setup_client(monkeypatch, tmp_path)
    with client:
        token = device_token(module)
        auth = {"Authorization": f"Bearer {token}"}
        photo = b"\xff\xd8\xff\x01demo\xff\xd9"
        checksum = hashlib.sha256(photo).hexdigest()
        upload_headers = {**auth, "X-Content-SHA256": checksum, "Content-Type": "image/jpeg"}
        url = "/api/device/photos/2026-10-17/capture-1.jpg"
        first = client.put(url, content=photo, headers=upload_headers)
        assert first.status_code == 200 and first.json()["duplicate"] is False
        duplicate = client.put(url, content=photo, headers=upload_headers)
        assert duplicate.status_code == 200 and duplicate.json()["duplicate"] is True
        other = b"\xff\xd8\xffdifferent\xff\xd9"
        conflict = client.put(url, content=other, headers={**auth, "X-Content-SHA256": hashlib.sha256(other).hexdigest()})
        assert conflict.status_code == 409
        assert client.put("/api/device/photos/2026-10-17/../../oops.jpg", content=photo, headers=upload_headers).status_code in {400, 404, 405}
        assert client.put("/api/device/photos/2026-10-17/bad.jpg", content=b"not jpeg", headers={**auth, "X-Content-SHA256": hashlib.sha256(b"not jpeg").hexdigest()}).status_code == 415
        assert client.get("/api/state").status_code == 401
        login(client)
        state = client.get("/api/state").json()
        assert state["analysis_enabled"] is False and len(state["photos"]) == 1
        assert state["photos"][0]["uploaded_at"] > 1_000_000_000_000
        assert client.get(state["photos"][0]["url"]).content == photo


def test_legacy_import_is_idempotent_and_keeps_source_and_analysis(monkeypatch, tmp_path):
    module, client = setup_client(monkeypatch, tmp_path)
    with client:
        source_db = tmp_path / "legacy.sqlite3"
        photo_root = tmp_path / "old-photos"
        image_path = photo_root / "plant-1" / "2026-10-17" / "old.jpg"
        image_path.parent.mkdir(parents=True)
        image_path.write_bytes(b"\xff\xd8\xfflegacy\xff\xd9")
        source = sqlite3.connect(source_db)
        source.executescript("""
          CREATE TABLE photos(station_id TEXT,date TEXT,filename TEXT,local_path TEXT,state TEXT,updated_at TEXT);
          CREATE TABLE analyses(station_id TEXT,date TEXT,filename TEXT,result_json TEXT,model TEXT,effort TEXT,passes INTEGER,response_id TEXT,input_tokens INTEGER,output_tokens INTEGER,completed_at TEXT);
        """)
        source.execute("INSERT INTO photos VALUES(?,?,?,?,?,?)", ("plant-1", "2026-10-17", "old.jpg", str(image_path), "downloaded", "2026-10-17T05:00:00+00:00"))
        source.execute("INSERT INTO analyses VALUES(?,?,?,?,?,?,?,?,?,?,?)", ("plant-1", "2026-10-17", "old.jpg", json.dumps({"count": 4}), "prior-model", "low", 1, "resp_123", 12, 3, "2026-10-17T05:01:00Z"))
        source.commit()
        source.close()
        import cli
        cli = importlib.reload(cli)
        cli.import_legacy(source_db, photo_root)
        cli.import_legacy(source_db, photo_root)
        with module.db() as conn:
            assert conn.execute("SELECT COUNT(*) FROM photos").fetchone()[0] == 1
            assert conn.execute("SELECT COUNT(*) FROM analysis_records").fetchone()[0] == 1
            row = conn.execute("SELECT record_json FROM analysis_records").fetchone()
            assert json.loads(row["record_json"])["response_id"] == "resp_123"
        login(client)
        analyses = client.get("/api/analyses").json()
        assert analyses["analysis_enabled"] is False
        assert analyses["analyses"][0]["record"]["response_id"] == "resp_123"
        assert image_path.read_bytes() == b"\xff\xd8\xfflegacy\xff\xd9"
        source = sqlite3.connect(f"file:{source_db.as_posix()}?mode=ro", uri=True)
        assert source.execute("SELECT COUNT(*) FROM photos").fetchone()[0] == 1
        source.close()


def test_ambiguous_analysis_submission_stays_uncertain_without_resubmit(monkeypatch, tmp_path):
    module, client = setup_client(monkeypatch, tmp_path)
    with client:
        import analysis_worker
        worker = importlib.reload(analysis_worker)
        photo = b"\xff\xd8\xffanalysis\xff\xd9"
        photo_path = module.PHOTO_DIR / "plant-1" / "2026-10-17" / "analysis.jpg"
        photo_path.parent.mkdir(parents=True)
        photo_path.write_bytes(photo)
        with module.db() as conn:
            relative = photo_path.relative_to(module.DATA_DIR)
            photo_id = conn.execute("INSERT INTO photos(station_id,date,filename,checksum,path,uploaded_at) VALUES(?,?,?,?,?,unixepoch())", ("plant-1", "2026-10-17", "analysis.jpg", hashlib.sha256(photo).hexdigest(), str(relative))).lastrowid
            conn.execute("INSERT INTO analysis_jobs(photo_id,state,model,passes,created_at,updated_at) VALUES(?,'queued','configured-test-model',1,unixepoch(),unixepoch())", (photo_id,))
        calls = []
        def ambiguous(*args, **kwargs):
            calls.append(args)
            raise TimeoutError("simulated response timeout")
        worker._http = ambiguous
        assert worker.process_queued_once() is True
        assert worker.process_queued_once() is False
        with module.db() as conn:
            job = conn.execute("SELECT state,response_id FROM analysis_jobs").fetchone()
        assert job["state"] == "uncertain" and job["response_id"] is None and len(calls) == 1


def test_analysis_worker_recovers_existing_response_id_without_resubmitting(monkeypatch, tmp_path):
    module, client = setup_client(monkeypatch, tmp_path)
    with client:
        import analysis_worker
        worker = importlib.reload(analysis_worker)
        photo = b"\xff\xd8\xffrecover\xff\xd9"
        photo_path = module.PHOTO_DIR / "plant-2" / "2026-10-17" / "recover.jpg"
        photo_path.parent.mkdir(parents=True)
        photo_path.write_bytes(photo)
        with module.db() as conn:
            relative = photo_path.relative_to(module.DATA_DIR)
            photo_id = conn.execute("INSERT INTO photos(station_id,date,filename,checksum,path,uploaded_at) VALUES(?,?,?,?,?,unixepoch())", ("plant-2", "2026-10-17", "recover.jpg", hashlib.sha256(photo).hexdigest(), str(relative))).lastrowid
            job_id = conn.execute("INSERT INTO analysis_jobs(photo_id,state,response_id,model,passes,created_at,updated_at) VALUES(?,'submitted','resp_saved','configured-test-model',1,1,1)", (photo_id,)).lastrowid
        result = {"water_level_cm": None, "final_tiller_count": 4, "tiller_check_counts": [4], "growth_stage": "active tillering", "water_confidence": 0.0, "tiller_confidence": 0.8, "growth_confidence": 0.7, "notes": "Visible basal shoots."}
        calls = []
        def recovered(method, url, *args, **kwargs):
            calls.append((method, url))
            return {"id": "resp_saved", "status": "completed", "output": [{"type": "message", "content": [{"type": "output_text", "text": json.dumps(result)}]}], "usage": {"input_tokens": 20, "output_tokens": 15}}
        worker._http = recovered
        assert worker.poll_submitted_once() is True
        with module.db() as conn:
            job = conn.execute("SELECT state,response_id,result_json FROM analysis_jobs WHERE id=?", (job_id,)).fetchone()
            analysis = conn.execute("SELECT record_json FROM analysis_records WHERE source_table='cloud_analysis'").fetchone()
        assert calls == [("GET", worker.API_URL + "/resp_saved")]
        assert job["state"] == "completed" and json.loads(job["result_json"]) == result
        assert json.loads(analysis["record_json"])["response_id"] == "resp_saved"


def test_automatic_analysis_queueing_and_duplicate_safety(monkeypatch, tmp_path):
    monkeypatch.setenv("RICE_DATA_DIR", str(tmp_path / "data"))
    monkeypatch.setenv("RICE_DB_PATH", str(tmp_path / "data" / "test.sqlite3"))
    monkeypatch.setenv("RICE_ADMIN_PASSWORD_HASH", make_hash())
    monkeypatch.setenv("RICE_COOKIE_SECURE", "false")
    monkeypatch.setenv("RICE_ANALYSIS_ENABLED", "true")
    monkeypatch.setenv("RICE_ANALYSIS_MODEL", "gpt-4o-mini")
    monkeypatch.setenv("RICE_OPENAI_API_KEY", "test-sk-rice-key")
    monkeypatch.delenv("RICE_PUBLIC_ORIGIN", raising=False)
    import analysis_worker
    monkeypatch.setattr(analysis_worker, "start_worker", lambda: None)
    import app as module
    module = importlib.reload(module)
    client = TestClient(module.app)
    with client:
        token = device_token(module, "plant-1")
        photo = b"\xff\xd8\xffautomatic\xff\xd9"
        checksum = hashlib.sha256(photo).hexdigest()
        headers = {"Authorization": f"Bearer {token}", "X-Content-SHA256": checksum, "Content-Type": "image/jpeg"}
        url = "/api/device/photos/2026-10-17/auto_01.jpg"

        # 1. New photo uploaded: immediately acknowledged and 1 analysis job queued asynchronously
        res1 = client.put(url, content=photo, headers=headers)
        assert res1.status_code == 200
        data1 = res1.json()
        assert data1["ok"] is True
        assert data1["duplicate"] is False
        photo_id = data1["photo_id"]

        with module.db() as conn:
            jobs = conn.execute("SELECT * FROM analysis_jobs").fetchall()
            assert len(jobs) == 1
            job = dict(jobs[0])
            assert job["photo_id"] == photo_id
            assert job["state"] == "queued"
            assert job["model"] == "gpt-4o-mini"
            assert job["passes"] == 1

        # 2. Duplicate upload: acknowledged with duplicate=True, does NOT queue a second job
        res2 = client.put(url, content=photo, headers=headers)
        assert res2.status_code == 200
        assert res2.json()["duplicate"] is True

        with module.db() as conn:
            jobs_after = conn.execute("SELECT * FROM analysis_jobs").fetchall()
            assert len(jobs_after) == 1

        # 3. GET /api/analysis/jobs requires admin session
        assert client.get("/api/analysis/jobs").status_code == 401
        login(client)
        jobs_res = client.get("/api/analysis/jobs")
        assert jobs_res.status_code == 200
        jobs_payload = jobs_res.json()
        assert jobs_payload["enabled"] is True
        assert len(jobs_payload["jobs"]) == 1
        j = jobs_payload["jobs"][0]
        assert j["photo_id"] == photo_id
        assert j["station_id"] == "plant-1"
        assert j["date"] == "2026-10-17"
        assert j["filename"] == "auto_01.jpg"
        assert j["state"] == "queued"
        assert j["model"] == "gpt-4o-mini"


def test_analysis_prompt_verification_wording():
    import analysis_worker
    worker = importlib.reload(analysis_worker)
    prompt1 = worker.analysis_prompt(1)
    prompt2 = worker.analysis_prompt(2)
    prompt3 = worker.analysis_prompt(3)
    for p in (prompt1, prompt2, prompt3):
        assert "independent visual tiller-counting passes" not in p
        assert "independent model samples" not in p
        assert "visual tiller-counting verification passes within this evaluation" in p
        assert "water_level_cm" not in p or "Measure water level in centimeters" in p


def test_explicit_retry_requeues_failed_but_never_uncertain_analysis(monkeypatch, tmp_path):
    monkeypatch.setenv("RICE_DATA_DIR", str(tmp_path / "data"))
    monkeypatch.setenv("RICE_DB_PATH", str(tmp_path / "data" / "test.sqlite3"))
    monkeypatch.setenv("RICE_ADMIN_PASSWORD_HASH", make_hash())
    monkeypatch.setenv("RICE_COOKIE_SECURE", "false")
    monkeypatch.setenv("RICE_ANALYSIS_ENABLED", "true")
    monkeypatch.setenv("RICE_ANALYSIS_MODEL", "configured-test-model")
    monkeypatch.setenv("RICE_OPENAI_API_KEY", "test-key")
    monkeypatch.delenv("RICE_PUBLIC_ORIGIN", raising=False)
    import analysis_worker
    monkeypatch.setattr(analysis_worker, "start_worker", lambda: None)
    import app as module
    module = importlib.reload(module)
    photo = b"\xff\xd8\xffretry\xff\xd9"
    photo_path = module.PHOTO_DIR / "plant-1" / "2026-10-17" / "retry.jpg"
    photo_path.parent.mkdir(parents=True)
    photo_path.write_bytes(photo)
    with TestClient(module.app) as client:
        with module.db() as conn:
            photo_id = conn.execute(
                "INSERT INTO photos(station_id,date,filename,checksum,path,uploaded_at) VALUES(?,?,?,?,?,unixepoch())",
                ("plant-1", "2026-10-17", "retry.jpg", hashlib.sha256(photo).hexdigest(), str(photo_path.relative_to(module.DATA_DIR))),
            ).lastrowid
            failed_id = conn.execute(
                "INSERT INTO analysis_jobs(photo_id,state,response_id,model,passes,result_json,usage_json,error,created_at,updated_at) VALUES(?,'failed','old-response','configured-test-model',1,'{}','{}','bad image',unixepoch(),unixepoch())",
                (photo_id,),
            ).lastrowid
            uncertain_photo_id = conn.execute(
                "INSERT INTO photos(station_id,date,filename,checksum,path,uploaded_at) VALUES(?,?,?,?,?,unixepoch())",
                ("plant-2", "2026-10-17", "uncertain.jpg", hashlib.sha256(photo).hexdigest(), str(photo_path.relative_to(module.DATA_DIR))),
            ).lastrowid
            uncertain_job_id = conn.execute(
                "INSERT INTO analysis_jobs(photo_id,state,model,passes,created_at,updated_at) VALUES(?,'uncertain','configured-test-model',1,unixepoch(),unixepoch())",
                (uncertain_photo_id,),
            ).lastrowid
        assert client.post(f"/api/analysis/jobs/{failed_id}/retry").status_code == 401
        csrf = login(client)
        headers = {"Origin": "http://testserver", "X-CSRF-Token": csrf}
        assert client.post(f"/api/analysis/jobs/{failed_id}/retry", headers=headers).json() == {"ok": True, "job_id": failed_id, "state": "queued"}
        assert client.post(f"/api/analysis/jobs/{uncertain_job_id}/retry", headers=headers).status_code == 409
        assert client.post("/api/analysis/jobs/999/retry", headers=headers).status_code == 404
    with module.db() as conn:
        job = conn.execute("SELECT state,response_id,result_json,usage_json,error FROM analysis_jobs WHERE id=?", (failed_id,)).fetchone()
        assert tuple(job) == ("queued", None, None, None, None)


def test_sd_archive_upload_never_triggers_paid_backfill(monkeypatch, tmp_path):
    module, client = setup_client(monkeypatch, tmp_path)
    client.__enter__()
    try:
        module.ANALYSIS_ENABLED = True
        token = device_token(module)
        photo = b"\xff\xd8\xff" + b"archived-image" + b"\xff\xd9"
        headers = {"Authorization": f"Bearer {token}", "X-Content-SHA256": hashlib.sha256(photo).hexdigest(), "X-Rice-Archive-Import": "true"}
        assert client.put("/api/device/photos/2026-09-14/archive.jpg", content=photo, headers=headers).status_code == 200
        with module.db() as conn:
            assert conn.execute("SELECT COUNT(*) FROM photos").fetchone()[0] == 1
            assert conn.execute("SELECT COUNT(*) FROM analysis_jobs").fetchone()[0] == 0
    finally:
        module.ANALYSIS_ENABLED = False
        client.__exit__(None, None, None)
