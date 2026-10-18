"""Small, durable FastAPI service for the three rice-monitoring stations."""
from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import time
from contextlib import asynccontextmanager, contextmanager
from pathlib import Path
from typing import Any

from fastapi import Depends, FastAPI, HTTPException, Request, Response
from fastapi.responses import FileResponse, JSONResponse
from pydantic import BaseModel, Field

BASE_DIR = Path(__file__).resolve().parent
DATA_DIR = Path(os.environ.get("RICE_DATA_DIR", BASE_DIR / "data")).resolve()
PHOTO_DIR = DATA_DIR / "photos"
DB_PATH = Path(os.environ.get("RICE_DB_PATH", DATA_DIR / "rice-cloud.sqlite3")).resolve()
STATIC_DIR = BASE_DIR / "static"
STATIONS = {"plant-1", "plant-2", "plant-3"}
MAX_PHOTO_BYTES = 16 * 1024 * 1024
SAFE_FILENAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,119}\.jpe?g$", re.I)
SAFE_DATE = re.compile(r"^(?:\d{4}-\d{2}-\d{2}|UNSORTED)$")
COOKIE_NAME = "rice_admin_session"
SESSION_SECONDS = int(os.environ.get("RICE_SESSION_SECONDS", "43200"))
COOKIE_SECURE = os.environ.get("RICE_COOKIE_SECURE", "true").lower() not in {"0", "false", "no"}
PUBLIC_ORIGIN = os.environ.get("RICE_PUBLIC_ORIGIN", "").rstrip("/")
ANALYSIS_ENABLED = os.environ.get("RICE_ANALYSIS_ENABLED", "false").lower() in {"1", "true", "yes"}
ANALYSIS_MODEL = os.environ.get("RICE_ANALYSIS_MODEL") or "gpt-6.1-sol"

@asynccontextmanager
async def lifespan(application: FastAPI):
    startup()
    try:
        yield
    finally:
        shutdown()


app = FastAPI(title="Rice Cloud", docs_url=None, redoc_url=None, openapi_url=None, lifespan=lifespan)


@contextmanager
def db() -> Any:
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    DB_PATH.parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(DB_PATH, timeout=20)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys=ON")
    connection.execute("PRAGMA journal_mode=WAL")
    connection.execute("PRAGMA synchronous=FULL")
    try:
        yield connection
        connection.commit()
    except Exception:
        connection.rollback()
        raise
    finally:
        connection.close()


def initialize() -> None:
    with db() as conn:
        conn.executescript("""
        CREATE TABLE IF NOT EXISTS schema_migrations(version INTEGER PRIMARY KEY, applied_at INTEGER NOT NULL);
        CREATE TABLE IF NOT EXISTS stations(
          station_id TEXT PRIMARY KEY, name TEXT NOT NULL, last_seen INTEGER,
          next_wake_at INTEGER, sleep_enabled INTEGER NOT NULL DEFAULT 0,
          awake INTEGER NOT NULL DEFAULT 0, status_json TEXT NOT NULL DEFAULT '{}',
          last_capture_json TEXT
        );
        CREATE TABLE IF NOT EXISTS device_tokens(
          token_hash TEXT PRIMARY KEY, station_id TEXT NOT NULL REFERENCES stations(station_id),
          created_at INTEGER NOT NULL, revoked_at INTEGER
        );
        CREATE TABLE IF NOT EXISTS sessions(
          token_hash TEXT PRIMARY KEY, csrf_token TEXT NOT NULL, created_at INTEGER NOT NULL,
          expires_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS login_attempts(
          client_key TEXT PRIMARY KEY, window_start INTEGER NOT NULL, failures INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS commands(
          id TEXT PRIMARY KEY, station_id TEXT NOT NULL REFERENCES stations(station_id),
          type TEXT NOT NULL, payload_json TEXT NOT NULL, state TEXT NOT NULL DEFAULT 'pending',
          created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL, message TEXT, result_json TEXT
        );
        CREATE INDEX IF NOT EXISTS commands_station_state ON commands(station_id,state,created_at);
        CREATE TABLE IF NOT EXISTS photos(
          id INTEGER PRIMARY KEY AUTOINCREMENT, station_id TEXT NOT NULL REFERENCES stations(station_id),
          date TEXT NOT NULL, filename TEXT NOT NULL, checksum TEXT NOT NULL, path TEXT NOT NULL,
          uploaded_at INTEGER NOT NULL, UNIQUE(station_id,date,filename)
        );
        CREATE TABLE IF NOT EXISTS analysis_records(
          id INTEGER PRIMARY KEY AUTOINCREMENT, source_table TEXT NOT NULL, source_key TEXT NOT NULL,
          station_id TEXT, date TEXT, filename TEXT, record_json TEXT NOT NULL,
          UNIQUE(source_table,source_key)
        );
        CREATE TABLE IF NOT EXISTS legacy_records(
          id INTEGER PRIMARY KEY AUTOINCREMENT, source_table TEXT NOT NULL,
          source_key TEXT NOT NULL, record_json TEXT NOT NULL,
          UNIQUE(source_table,source_key)
        );
        CREATE TABLE IF NOT EXISTS analysis_jobs(
          id INTEGER PRIMARY KEY AUTOINCREMENT, photo_id INTEGER NOT NULL UNIQUE REFERENCES photos(id),
          state TEXT NOT NULL, response_id TEXT, model TEXT NOT NULL, passes INTEGER NOT NULL,
          result_json TEXT, usage_json TEXT, error TEXT, created_at INTEGER NOT NULL, updated_at INTEGER NOT NULL
        );
        INSERT OR IGNORE INTO schema_migrations(version,applied_at) VALUES(1,unixepoch());
        INSERT OR IGNORE INTO stations(station_id,name) VALUES
          ('plant-1','Plant 1'),('plant-2','Plant 2'),('plant-3','Plant 3');
        """)
        command_columns = {row[1] for row in conn.execute("PRAGMA table_info(commands)")}
        if "result_json" not in command_columns:
            conn.execute("ALTER TABLE commands ADD COLUMN result_json TEXT")
        conn.execute("INSERT OR IGNORE INTO schema_migrations(version,applied_at) VALUES(2,unixepoch())")


def startup() -> None:
    configured_hash = os.environ.get("RICE_ADMIN_PASSWORD_HASH", "")
    if not configured_hash:
        raise RuntimeError("RICE_ADMIN_PASSWORD_HASH must be configured before starting Rice Cloud")
    parse_password_hash(configured_hash)
    initialize()
    PHOTO_DIR.mkdir(parents=True, exist_ok=True)
    if ANALYSIS_ENABLED:
        if not os.environ.get("RICE_OPENAI_API_KEY") or not ANALYSIS_MODEL:
            raise RuntimeError("RICE_OPENAI_API_KEY and RICE_ANALYSIS_MODEL are required when analysis is enabled")
        try:
            passes = int(os.environ.get("RICE_ANALYSIS_PASSES", "1"))
        except ValueError as exc:
            raise RuntimeError("RICE_ANALYSIS_PASSES must be 1, 2, or 3") from exc
        if passes not in {1, 2, 3}:
            raise RuntimeError("RICE_ANALYSIS_PASSES must be 1, 2, or 3")
        from analysis_worker import start_worker, stale_submissions_to_uncertain
        stale_submissions_to_uncertain()
        start_worker()


def shutdown() -> None:
    if ANALYSIS_ENABLED:
        from analysis_worker import stop_worker
        stop_worker()


def token_hash(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def admin_timestamp(value: int | None) -> int | None:
    """Admin UI timestamps use JavaScript Date's millisecond convention."""
    return value * 1000 if value is not None else None


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def parse_password_hash(value: str) -> tuple[int, bytes, bytes]:
    """Expected form: pbkdf2_sha256$iterations$salt_hex$digest_hex."""
    try:
        algorithm, iterations, salt, digest = value.split("$", 3)
        if algorithm != "pbkdf2_sha256":
            raise ValueError("unsupported password hash")
        rounds = int(iterations)
        salt_bytes, digest_bytes = bytes.fromhex(salt), bytes.fromhex(digest)
        if not 200_000 <= rounds <= 2_000_000 or len(salt_bytes) < 16 or len(digest_bytes) != 32:
            raise ValueError("invalid password hash parameters")
        return rounds, salt_bytes, digest_bytes
    except (ValueError, TypeError) as exc:
        raise RuntimeError("RICE_ADMIN_PASSWORD_HASH must be a valid PBKDF2-SHA256 hash") from exc


def verify_admin_password(password: str) -> bool:
    configured = os.environ.get("RICE_ADMIN_PASSWORD_HASH", "")
    if not configured:
        return False
    rounds, salt, expected = parse_password_hash(configured)
    actual = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt, rounds)
    return hmac.compare_digest(actual, expected)


def admin_session(request: Request) -> sqlite3.Row:
    value = request.cookies.get(COOKIE_NAME, "")
    if not value:
        raise HTTPException(401, "Login required")
    with db() as conn:
        row = conn.execute("SELECT * FROM sessions WHERE token_hash=? AND expires_at>?", (token_hash(value), int(time.time()))).fetchone()
    if row is None:
        raise HTTPException(401, "Login required")
    return row


def require_admin(request: Request) -> sqlite3.Row:
    return admin_session(request)


def require_csrf(request: Request, session: sqlite3.Row = Depends(require_admin)) -> sqlite3.Row:
    origin = request.headers.get("origin", "").rstrip("/")
    expected = PUBLIC_ORIGIN or f"{request.url.scheme}://{request.url.netloc}"
    if not origin or not hmac.compare_digest(origin, expected):
        raise HTTPException(403, "Same-origin request required")
    supplied = request.headers.get("x-csrf-token", "")
    if not supplied or not hmac.compare_digest(supplied, session["csrf_token"]):
        raise HTTPException(403, "Invalid CSRF token")
    return session


def device_station(request: Request) -> str:
    header = request.headers.get("authorization", "")
    scheme, _, token = header.partition(" ")
    if scheme.lower() != "bearer" or not token:
        raise HTTPException(401, "Device token required")
    with db() as conn:
        row = conn.execute("SELECT station_id FROM device_tokens WHERE token_hash=? AND revoked_at IS NULL", (token_hash(token),)).fetchone()
    if row is None:
        raise HTTPException(401, "Invalid device token")
    return row["station_id"]


class LoginBody(BaseModel):
    password: str = Field(min_length=1, max_length=1024)


class SyncBody(BaseModel):
    station_id: str
    status: dict[str, Any] = Field(default_factory=dict)
    next_wake_at: int | None = None
    sleep_enabled: bool = False
    awake: bool = False
    last_capture: dict[str, Any] | None = None


class CommandBody(BaseModel):
    type: str
    payload: dict[str, Any] = Field(default_factory=dict)


class CommandResult(BaseModel):
    state: str
    message: str | None = Field(default=None, max_length=1000)
    photo: dict[str, Any] | None = None


@app.get("/api/health")
def health() -> dict[str, bool]:
    return {"ok": True}


@app.get("/api/session")
def session_status(request: Request) -> dict[str, Any]:
    try:
        session = admin_session(request)
        return {"authenticated": True, "csrf_token": session["csrf_token"]}
    except HTTPException as exc:
        if exc.status_code != 401:
            raise
        return {"authenticated": False, "csrf_token": None}


@app.post("/api/login")
def login(body: LoginBody, request: Request, response: Response) -> dict[str, Any]:
    now = int(time.time())
    client = request.client.host if request.client else "unknown"
    key = token_hash(client)
    rate_limited = False
    valid = False
    raw = csrf = ""
    with db() as conn:
        attempt = conn.execute("SELECT * FROM login_attempts WHERE client_key=?", (key,)).fetchone()
        if attempt and now - attempt["window_start"] < 900 and attempt["failures"] >= 8:
            rate_limited = True
        elif os.environ.get("RICE_ADMIN_PASSWORD_HASH"):
            valid = verify_admin_password(body.password)
        if not rate_limited and not valid:
            if attempt and now - attempt["window_start"] < 900:
                conn.execute("UPDATE login_attempts SET failures=failures+1 WHERE client_key=?", (key,))
            else:
                conn.execute("INSERT OR REPLACE INTO login_attempts(client_key,window_start,failures) VALUES(?,?,1)", (key, now))
        elif valid:
            conn.execute("DELETE FROM login_attempts WHERE client_key=?", (key,))
            raw = secrets.token_urlsafe(32)
            csrf = secrets.token_urlsafe(32)
            conn.execute("INSERT INTO sessions(token_hash,csrf_token,created_at,expires_at) VALUES(?,?,?,?)", (token_hash(raw), csrf, now, now + SESSION_SECONDS))
    if rate_limited:
        raise HTTPException(429, "Too many login attempts")
    if not valid:
        raise HTTPException(401, "Invalid password")
    response.set_cookie(COOKIE_NAME, raw, httponly=True, secure=COOKIE_SECURE, samesite="strict", max_age=SESSION_SECONDS, path="/")
    return {"authenticated": True, "csrf_token": csrf}


@app.post("/api/logout")
def logout(request: Request, response: Response, session: sqlite3.Row = Depends(require_csrf)) -> dict[str, bool]:
    raw = request.cookies.get(COOKIE_NAME, "")
    with db() as conn:
        conn.execute("DELETE FROM sessions WHERE token_hash=?", (token_hash(raw),))
    response.delete_cookie(COOKIE_NAME, path="/", secure=COOKIE_SECURE, httponly=True, samesite="strict")
    return {"ok": True}


def pending_commands(conn: sqlite3.Connection, station_id: str) -> list[dict[str, Any]]:
    rows = conn.execute("SELECT id,type,payload_json FROM commands WHERE station_id=? AND state='pending' ORDER BY created_at LIMIT 8", (station_id,)).fetchall()
    return [{"id": row["id"], "type": row["type"], "payload": json.loads(row["payload_json"])} for row in rows]


@app.post("/api/device/sync")
def device_sync(body: SyncBody, request: Request) -> dict[str, Any]:
    station_id = device_station(request)
    if body.station_id != station_id:
        raise HTTPException(403, "Token is bound to a different station")
    if len(json.dumps(body.status, separators=(",", ":"))) > 64 * 1024:
        raise HTTPException(413, "Station status exceeds 64 KiB")
    now = int(time.time())
    with db() as conn:
        conn.execute("UPDATE stations SET last_seen=?,next_wake_at=?,sleep_enabled=?,awake=?,status_json=?,last_capture_json=? WHERE station_id=?", (now, body.next_wake_at, int(body.sleep_enabled), int(body.awake), json.dumps(body.status, separators=(",", ":")), json.dumps(body.last_capture, separators=(",", ":")) if body.last_capture is not None else None, station_id))
        commands = pending_commands(conn, station_id)
    return {"ok": True, "server_time": now, "commands": commands}


@app.get("/api/device/commands")
def get_device_commands(request: Request) -> dict[str, Any]:
    station_id = device_station(request)
    with db() as conn:
        return {"commands": pending_commands(conn, station_id)}


@app.post("/api/device/commands/{command_id}/result")
def command_result(command_id: str, body: CommandResult, request: Request) -> dict[str, bool]:
    station_id = device_station(request)
    if body.state not in {"completed", "failed", "uncertain"}:
        raise HTTPException(422, "state must be completed, failed, or uncertain")
    if body.photo is not None:
        date, filename = body.photo.get("date"), body.photo.get("filename")
        if not isinstance(date, str) or not isinstance(filename, str) or not SAFE_DATE.fullmatch(date) or not SAFE_FILENAME.fullmatch(filename):
            raise HTTPException(422, "Invalid photo identity")
    with db() as conn:
        result = conn.execute("UPDATE commands SET state=?,message=?,result_json=?,updated_at=? WHERE id=? AND station_id=? AND state='pending'", (body.state, body.message, json.dumps(body.model_dump(), separators=(",", ":")), int(time.time()), command_id, station_id))
        if result.rowcount == 0:
            row = conn.execute("SELECT state FROM commands WHERE id=? AND station_id=?", (command_id, station_id)).fetchone()
            if row is None:
                raise HTTPException(404, "Command not found")
            if row["state"] not in {"completed", "failed", "uncertain"}:
                raise HTTPException(409, "Command is no longer pending")
    return {"ok": True}


def validate_photo_identity(date: str, filename: str) -> None:
    if not SAFE_DATE.fullmatch(date) or not SAFE_FILENAME.fullmatch(filename):
        raise HTTPException(400, "Invalid photo path")


@app.put("/api/device/photos/{date}/{filename}")
async def upload_photo(date: str, filename: str, request: Request) -> dict[str, Any]:
    station_id = device_station(request)
    validate_photo_identity(date, filename)
    expected = request.headers.get("x-content-sha256", "").lower()
    if not re.fullmatch(r"[a-f0-9]{64}", expected):
        raise HTTPException(400, "X-Content-SHA256 must contain a SHA-256 hex digest")
    declared_length = request.headers.get("content-length")
    if declared_length:
        try:
            if int(declared_length) > MAX_PHOTO_BYTES:
                raise HTTPException(413, "Photo exceeds 16 MiB limit")
        except ValueError as exc:
            raise HTTPException(400, "Invalid Content-Length") from exc
    staging_dir = PHOTO_DIR / ".staging"
    staging_dir.mkdir(parents=True, exist_ok=True)
    temp_path = staging_dir / secrets.token_hex(16)
    digest = hashlib.sha256()
    size = 0
    first = b""
    tail = b""
    try:
        with temp_path.open("xb") as output:
            async for chunk in request.stream():
                size += len(chunk)
                if size > MAX_PHOTO_BYTES:
                    raise HTTPException(413, "Photo exceeds 16 MiB limit")
                if len(first) < 3:
                    first += chunk[:3 - len(first)]
                if chunk:
                    tail = (tail + chunk)[-2:]
                digest.update(chunk)
                output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        if size < 4 or not first.startswith(b"\xff\xd8\xff") or tail != b"\xff\xd9":
            raise HTTPException(415, "Upload is not a complete JPEG")
        actual = digest.hexdigest()
        if not hmac.compare_digest(actual, expected):
            raise HTTPException(400, "Photo checksum does not match")
        relative = Path("photos") / station_id / date / filename
        target = DATA_DIR / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        with db() as conn:
            conn.execute("BEGIN IMMEDIATE")
            existing = conn.execute("SELECT checksum,id FROM photos WHERE station_id=? AND date=? AND filename=?", (station_id, date, filename)).fetchone()
            if existing:
                if not hmac.compare_digest(existing["checksum"], actual):
                    raise HTTPException(409, "Photo identity already exists with a different checksum")
                if not target.is_file() or not hmac.compare_digest(_file_sha256(target), actual):
                    os.replace(temp_path, target)
                    if os.name != "nt":
                        directory_fd = os.open(target.parent, os.O_RDONLY)
                        try:
                            os.fsync(directory_fd)
                        finally:
                            os.close(directory_fd)
                return {"ok": True, "photo_id": existing["id"], "duplicate": True}
            if target.exists():
                if not hmac.compare_digest(_file_sha256(target), actual):
                    raise HTTPException(409, "Unindexed photo path already contains different data")
                temp_path.unlink(missing_ok=True)
            else:
                os.replace(temp_path, target)
                if os.name != "nt":
                    directory_fd = os.open(target.parent, os.O_RDONLY)
                    try:
                        os.fsync(directory_fd)
                    finally:
                        os.close(directory_fd)
            uploaded_at = int(time.time())
            cursor = conn.execute("INSERT INTO photos(station_id,date,filename,checksum,path,uploaded_at) VALUES(?,?,?,?,?,?)", (station_id, date, filename, actual, str(relative), uploaded_at))
            if ANALYSIS_ENABLED and request.headers.get("x-rice-archive-import", "").lower() != "true":
                passes = int(os.environ.get("RICE_ANALYSIS_PASSES", "1"))
                if passes not in {1, 2, 3}:
                    raise HTTPException(503, "RICE_ANALYSIS_PASSES must be 1, 2, or 3")
                conn.execute("INSERT INTO analysis_jobs(photo_id,state,model,passes,created_at,updated_at) VALUES(?,'queued',?,?,?,?)", (cursor.lastrowid, ANALYSIS_MODEL, passes, uploaded_at, uploaded_at))
            return {"ok": True, "photo_id": cursor.lastrowid, "duplicate": False}
    finally:
        try:
            temp_path.unlink(missing_ok=True)
        except OSError:
            pass


@app.get("/api/state")
def admin_state(_: sqlite3.Row = Depends(require_admin)) -> dict[str, Any]:
    with db() as conn:
        stations = conn.execute("SELECT * FROM stations ORDER BY station_id").fetchall()
        photos = conn.execute("SELECT id,station_id,date,filename,uploaded_at FROM photos ORDER BY uploaded_at DESC").fetchall()
        station_data = []
        for row in stations:
            latest = conn.execute("SELECT date,filename,uploaded_at FROM photos WHERE station_id=? ORDER BY uploaded_at DESC LIMIT 1", (row["station_id"],)).fetchone()
            latest_payload = dict(latest) if latest else None
            if latest_payload:
                latest_payload["uploaded_at"] = admin_timestamp(latest_payload["uploaded_at"])
            station_data.append({"station_id": row["station_id"], "name": row["name"], "last_seen": admin_timestamp(row["last_seen"]), "next_wake_at": admin_timestamp(row["next_wake_at"]), "sleep_enabled": bool(row["sleep_enabled"]), "awake": bool(row["awake"]), "status": json.loads(row["status_json"]), "last_capture": json.loads(row["last_capture_json"]) if row["last_capture_json"] else None, "latest_photo": latest_payload, "pending_commands": pending_commands(conn, row["station_id"])})
    return {"stations": station_data, "photos": [{"id": p["id"], "station_id": p["station_id"], "date": p["date"], "filename": p["filename"], "url": f"/api/photos/{p['id']}", "uploaded_at": admin_timestamp(p["uploaded_at"])} for p in photos], "analysis_enabled": ANALYSIS_ENABLED}


@app.get("/api/analyses")
def admin_analyses(_: sqlite3.Row = Depends(require_admin)) -> dict[str, Any]:
    """Read-only archive of analysis records imported from the former local hub."""
    with db() as conn:
        rows = conn.execute("SELECT id,source_table,source_key,station_id,date,filename,record_json FROM analysis_records ORDER BY id DESC").fetchall()
    return {"analyses": [{"id": r["id"], "source": r["source_table"], "source_key": r["source_key"], "station_id": r["station_id"], "date": r["date"], "filename": r["filename"], "record": json.loads(r["record_json"])} for r in rows], "analysis_enabled": ANALYSIS_ENABLED}


@app.get("/api/analysis/jobs")
def analysis_jobs(_: sqlite3.Row = Depends(require_admin)) -> dict[str, Any]:
    with db() as conn:
        rows = conn.execute("SELECT j.id,p.id AS photo_id,p.station_id,p.date,p.filename,j.state,j.response_id,j.model,j.passes,j.result_json,j.usage_json,j.error,j.created_at,j.updated_at FROM analysis_jobs j JOIN photos p ON p.id=j.photo_id ORDER BY j.created_at DESC").fetchall()
    return {"enabled": ANALYSIS_ENABLED, "jobs": [{"id": r["id"], "photo_id": r["photo_id"], "station_id": r["station_id"], "date": r["date"], "filename": r["filename"], "state": r["state"], "response_id": r["response_id"], "model": r["model"], "passes": r["passes"], "result": json.loads(r["result_json"]) if r["result_json"] else None, "usage": json.loads(r["usage_json"]) if r["usage_json"] else None, "error": r["error"], "created_at": admin_timestamp(r["created_at"]), "updated_at": admin_timestamp(r["updated_at"])} for r in rows]}


@app.post("/api/analysis/jobs/{job_id}/retry")
def retry_analysis_job(job_id: int, _: sqlite3.Row = Depends(require_csrf)) -> dict[str, Any]:
    """Explicitly requeue a failed job; uncertain submissions are never retried."""
    if not ANALYSIS_ENABLED:
        raise HTTPException(409, "Cloud analysis is disabled")
    with db() as conn:
        result = conn.execute(
            "UPDATE analysis_jobs SET state='queued',response_id=NULL,result_json=NULL,usage_json=NULL,error=NULL,updated_at=? WHERE id=? AND state='failed'",
            (int(time.time()), job_id),
        )
        if result.rowcount == 0:
            row = conn.execute("SELECT state FROM analysis_jobs WHERE id=?", (job_id,)).fetchone()
            if row is None:
                raise HTTPException(404, "Analysis job not found")
            if row["state"] == "uncertain":
                raise HTTPException(409, "Submission outcome is uncertain; this job cannot be retried automatically")
            raise HTTPException(409, "Only failed analysis jobs can be retried")
    return {"ok": True, "job_id": job_id, "state": "queued"}


@app.get("/api/photos/{photo_id}")
def get_photo(photo_id: int, _: sqlite3.Row = Depends(require_admin)) -> FileResponse:
    with db() as conn:
        row = conn.execute("SELECT path,filename FROM photos WHERE id=?", (photo_id,)).fetchone()
    if not row:
        raise HTTPException(404, "Photo not found")
    path = (DATA_DIR / row["path"]).resolve()
    if DATA_DIR not in path.parents or not path.is_file():
        raise HTTPException(404, "Photo not found")
    return FileResponse(path, media_type="image/jpeg", filename=row["filename"], content_disposition_type="inline")


ALLOWED_COMMANDS = {"take_photo", "stay_awake", "resume_sleep", "set_settings"}


@app.post("/api/stations/{station_id}/commands")
def create_command(station_id: str, body: CommandBody, _: sqlite3.Row = Depends(require_csrf)) -> dict[str, Any]:
    if station_id not in STATIONS or body.type not in ALLOWED_COMMANDS:
        raise HTTPException(400, "Invalid station or command type")
    if len(json.dumps(body.payload, separators=(",", ":"))) > 64 * 1024:
        raise HTTPException(413, "Command payload exceeds 64 KiB")
    if body.type == "set_settings":
        expected = {"sleep_enabled", "timer_enabled", "interval_seconds", "window_enabled", "window_start", "window_end"}
        payload = body.payload
        if set(payload) != expected or any(type(payload[key]) is not bool for key in ("sleep_enabled", "timer_enabled", "window_enabled")):
            raise HTTPException(422, "set_settings requires all settings with boolean flags")
        interval = payload["interval_seconds"]
        if type(interval) is not int or not 3600 <= interval <= 604800:
            raise HTTPException(422, "interval_seconds must be between 3600 and 604800")
        if any(not isinstance(payload[key], str) or not re.fullmatch(r"(?:[01]\d|2[0-3]):[0-5]\d", payload[key]) for key in ("window_start", "window_end")):
            raise HTTPException(422, "window_start and window_end must use HH:MM")
    now, command_id = int(time.time()), secrets.token_urlsafe(16)
    with db() as conn:
        count = conn.execute("SELECT COUNT(*) FROM commands WHERE station_id=? AND state='pending'", (station_id,)).fetchone()[0]
        if count >= 8:
            raise HTTPException(409, "Station command queue is full")
        conn.execute("INSERT INTO commands(id,station_id,type,payload_json,created_at,updated_at) VALUES(?,?,?,?,?,?)", (command_id, station_id, body.type, json.dumps(body.payload, separators=(",", ":")), now, now))
    return {"id": command_id, "type": body.type, "payload": body.payload, "state": "pending", "created_at": now}


@app.post("/api/stations/{station_id}/commands/{command_id}/cancel")
def cancel_command(station_id: str, command_id: str, _: sqlite3.Row = Depends(require_csrf)) -> dict[str, bool]:
    with db() as conn:
        result = conn.execute("UPDATE commands SET state='cancelled',updated_at=? WHERE station_id=? AND id=? AND state='pending'", (int(time.time()), station_id, command_id))
        if result.rowcount == 0:
            row = conn.execute("SELECT state FROM commands WHERE station_id=? AND id=?", (station_id, command_id)).fetchone()
            if row is None:
                raise HTTPException(404, "Command not found")
            raise HTTPException(409, "Only pending commands can be cancelled")
    return {"ok": True}


@app.get("/", include_in_schema=False)
def index() -> Response:
    path = STATIC_DIR / "index.html"
    if path.is_file():
        return FileResponse(path)
    return JSONResponse({"service": "Rice Cloud", "login": "/api/login", "health": "/api/health"})


@app.get("/{asset_path:path}", include_in_schema=False)
def static_asset(asset_path: str) -> Response:
    path = (STATIC_DIR / asset_path).resolve()
    if STATIC_DIR not in path.parents or not path.is_file():
        raise HTTPException(404, "Not found")
    return FileResponse(path)
