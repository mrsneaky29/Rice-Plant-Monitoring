from __future__ import annotations

import base64
import json
import mimetypes
import os
import re
import shutil
import socket
import sqlite3
import sys
import threading
import time
import webbrowser
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path

APP_DIR = (
    Path(sys.executable).resolve().parent
    if getattr(sys, "frozen", False)
    else Path(__file__).resolve().parent
)
BUNDLE_DIR = Path(getattr(sys, "_MEIPASS", APP_DIR))
DATA_DIR = APP_DIR / "data"
PHOTO_DIR = DATA_DIR / "photos"
BACKUP_DIR = DATA_DIR / "backups"
STATIC_DIR = BUNDLE_DIR / "static"
DB_PATH = DATA_DIR / "monitoring.sqlite"
API_KEY_FILE = DATA_DIR / "openai_key.txt"
UDP_PORT = 4210
HTTP_PORT = 8765
OPENAI_URL = "https://api.openai.com/v1/responses"
MODELS = ("gpt-5.6-sol", "gpt-5.6-terra", "gpt-5.6-luna")
EFFORTS = ("none", "low", "medium", "high", "xhigh", "max")
CAMERA_ONLY_MODE = True

stop_event = threading.Event()
sync_event = threading.Event()
db_lock = threading.RLock()


def now_iso():
    return datetime.now().astimezone().isoformat(timespec="seconds")


class ClosingConnection(sqlite3.Connection):
    def __exit__(self, exc_type, exc_value, traceback):
        try:
            return super().__exit__(exc_type, exc_value, traceback)
        finally:
            self.close()


def db():
    con = sqlite3.connect(DB_PATH, timeout=30, factory=ClosingConnection)
    con.row_factory = sqlite3.Row
    con.execute("PRAGMA journal_mode=WAL")
    con.execute("PRAGMA synchronous=FULL")
    con.execute("PRAGMA foreign_keys=ON")
    return con


def ensure_dirs():
    for path in (DATA_DIR, PHOTO_DIR, BACKUP_DIR, STATIC_DIR):
        path.mkdir(parents=True, exist_ok=True)
    for partial in PHOTO_DIR.rglob("*.part"):
        try:
            partial.unlink()
        except OSError:
            pass


def check_and_restore_database():
    if not DB_PATH.exists():
        return
    try:
        con = sqlite3.connect(DB_PATH)
        result = con.execute("PRAGMA integrity_check").fetchone()[0]
        con.close()
        if result == "ok":
            return
    except sqlite3.DatabaseError:
        pass
    candidates = sorted(BACKUP_DIR.glob("monitoring-*.sqlite"), reverse=True)
    corrupt = DB_PATH.with_suffix(f".corrupt-{int(time.time())}.sqlite")
    shutil.move(DB_PATH, corrupt)
    for candidate in candidates:
        try:
            test = sqlite3.connect(candidate)
            ok = test.execute("PRAGMA integrity_check").fetchone()[0] == "ok"
            test.close()
            if ok:
                shutil.copy2(candidate, DB_PATH)
                return
        except sqlite3.DatabaseError:
            continue


def initialize_database():
    schema = """
    CREATE TABLE IF NOT EXISTS settings(key TEXT PRIMARY KEY, value TEXT NOT NULL);
    CREATE TABLE IF NOT EXISTS stations(
      station_id TEXT PRIMARY KEY, station_no INTEGER, name TEXT, role TEXT,
      ip TEXT, mac TEXT, last_seen TEXT, online INTEGER DEFAULT 0,
      status_json TEXT DEFAULT '{}');
    CREATE TABLE IF NOT EXISTS photos(
      station_id TEXT NOT NULL, date TEXT NOT NULL, filename TEXT NOT NULL,
      size INTEGER DEFAULT 0, state TEXT NOT NULL DEFAULT 'discovered',
      local_path TEXT, response_id TEXT, sensor_reading_id INTEGER, analyze INTEGER NOT NULL DEFAULT 1, attempts INTEGER DEFAULT 0,
      error TEXT, model TEXT, effort TEXT, passes INTEGER,
      created_at TEXT NOT NULL, updated_at TEXT NOT NULL,
      PRIMARY KEY(station_id,date,filename));
    CREATE TABLE IF NOT EXISTS sensor_readings(
      id INTEGER PRIMARY KEY AUTOINCREMENT, station_id TEXT NOT NULL,
      date TEXT NOT NULL, reading_time TEXT NOT NULL, role TEXT NOT NULL,
      temperature_c REAL, humidity_percent REAL, soil_volts REAL, soil_percent REAL,
      UNIQUE(station_id,date,reading_time,role));
    CREATE TABLE IF NOT EXISTS analyses(
      station_id TEXT NOT NULL, date TEXT NOT NULL, filename TEXT NOT NULL,
      result_json TEXT NOT NULL, model TEXT NOT NULL, effort TEXT NOT NULL,
      passes INTEGER NOT NULL, response_id TEXT, input_tokens INTEGER DEFAULT 0,
      output_tokens INTEGER DEFAULT 0, completed_at TEXT NOT NULL,
      PRIMARY KEY(station_id,date,filename));
    CREATE TABLE IF NOT EXISTS analysis_history(
      id INTEGER PRIMARY KEY AUTOINCREMENT, station_id TEXT NOT NULL,
      date TEXT NOT NULL, filename TEXT NOT NULL, result_json TEXT NOT NULL,
      model TEXT NOT NULL, effort TEXT NOT NULL, passes INTEGER NOT NULL,
      response_id TEXT, input_tokens INTEGER DEFAULT 0, output_tokens INTEGER DEFAULT 0,
      completed_at TEXT NOT NULL, archived_at TEXT NOT NULL);
    CREATE TABLE IF NOT EXISTS event_log(
      id INTEGER PRIMARY KEY AUTOINCREMENT, created_at TEXT NOT NULL,
      level TEXT NOT NULL, message TEXT NOT NULL);
    """
    defaults = {
        "model": "gpt-5.6-sol",
        "effort": "low",
        "passes": "1",
        "analysis_cutoff": now_iso(),
    }
    with db_lock, db() as con:
        con.executescript(schema)
        for key, value in defaults.items():
            con.execute("INSERT OR IGNORE INTO settings(key,value) VALUES(?,?)", (key, value))
        columns = {row[1] for row in con.execute("PRAGMA table_info(photos)")}
        if "sensor_reading_id" not in columns:
            con.execute("ALTER TABLE photos ADD COLUMN sensor_reading_id INTEGER")
        if "analyze" not in columns:
            con.execute("ALTER TABLE photos ADD COLUMN analyze INTEGER NOT NULL DEFAULT 1")
        marker = con.execute("SELECT value FROM settings WHERE key='new_photos_only_v1'").fetchone()
        if not marker:
            con.execute(
                "UPDATE photos SET analyze=0,state=CASE WHEN state='downloaded' THEN 'archived' ELSE state END WHERE state!='completed'"
            )
            con.execute("INSERT INTO settings(key,value) VALUES('new_photos_only_v1','1')")
        for number in (1, 2, 3):
            con.execute(
                """INSERT OR IGNORE INTO stations
              (station_id,station_no,name,role,online,status_json)
              VALUES(?,?,?,?,0,'{}')""",
                (f"plant-{number}", number, f"Plant {number}", "camera"),
            )
        if CAMERA_ONLY_MODE:
            con.execute("UPDATE stations SET role='camera'")


def make_backup():
    if not DB_PATH.exists():
        return
    stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    target = BACKUP_DIR / f"monitoring-{stamp}.sqlite"
    try:
        with db_lock:
            src = db()
            dst = sqlite3.connect(target)
            src.backup(dst)
            dst.close()
            src.close()
        for old in sorted(BACKUP_DIR.glob("monitoring-*.sqlite"), reverse=True)[7:]:
            old.unlink(missing_ok=True)
    except (OSError, sqlite3.DatabaseError):
        target.unlink(missing_ok=True)


def backup_worker():
    while not stop_event.wait(3600):
        make_backup()


def log(message, level="info"):
    print(f"[{level}] {message}", flush=True)
    try:
        with db_lock, db() as con:
            con.execute(
                "INSERT INTO event_log(created_at,level,message) VALUES(?,?,?)",
                (now_iso(), level, message[:1000]),
            )
    except sqlite3.Error:
        pass


def get_setting(key, default=""):
    with db_lock, db() as con:
        row = con.execute("SELECT value FROM settings WHERE key=?", (key,)).fetchone()
    return row[0] if row else default


def set_setting(key, value):
    with db_lock, db() as con:
        con.execute(
            "INSERT INTO settings(key,value) VALUES(?,?) ON CONFLICT(key) DO UPDATE SET value=excluded.value",
            (key, str(value)),
        )


def api_key():
    if os.environ.get("OPENAI_API_KEY", "").strip():
        return os.environ["OPENAI_API_KEY"].strip()
    try:
        return API_KEY_FILE.read_text(encoding="utf-8").strip()
    except OSError:
        return ""


def save_api_key(value):
    DATA_DIR.mkdir(parents=True, exist_ok=True)
    API_KEY_FILE.write_text(value.strip(), encoding="utf-8")


def url_json(url, timeout=8):
    req = urllib.request.Request(url, headers={"User-Agent": "RiceMonitoringHub/1.0"})
    with urllib.request.urlopen(req, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def upsert_station(payload, source_ip=None):
    sid = str(payload.get("station_id") or "").strip()
    if sid not in {"plant-1", "plant-2", "plant-3"}:
        return
    ip = str(payload.get("ip") or source_ip or "").strip()
    with db_lock, db() as con:
        con.execute(
            """
          INSERT INTO stations(station_id,station_no,name,role,ip,mac,last_seen,online,status_json)
          VALUES(?,?,?,?,?,?,?,?,?)
          ON CONFLICT(station_id) DO UPDATE SET
            station_no=excluded.station_no,name=excluded.name,role=excluded.role,
            ip=excluded.ip,mac=excluded.mac,last_seen=excluded.last_seen,online=1
        """,
            (
                sid,
                int(payload.get("station_no") or sid[-1]),
                payload.get("name") or sid,
                "camera" if CAMERA_ONLY_MODE else (payload.get("role") or "none"),
                ip,
                payload.get("mac") or "",
                now_iso(),
                1,
                "{}",
            ),
        )


def broadcast(sock, message):
    data = json.dumps(message, separators=(",", ":")).encode()
    sock.sendto(data, ("255.255.255.255", UDP_PORT))


def discovery_worker():
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.bind(("", UDP_PORT))
    sock.settimeout(2)
    next_announce = 0
    while not stop_event.is_set():
        if time.time() >= next_announce:
            try:
                broadcast(
                    sock, {"type": "PC_DISCOVER", "service": "rice-monitor", "port": HTTP_PORT}
                )
            except OSError:
                pass
            next_announce = time.time() + 60
        try:
            raw, addr = sock.recvfrom(4096)
            payload = json.loads(raw.decode("utf-8"))
            if payload.get("type") in {"STATION_ANNOUNCE", "STATION_PING"}:
                upsert_station(payload, addr[0])
                broadcast(sock, {"type": "PC_ONLINE", "service": "rice-monitor", "port": HTTP_PORT})
                sync_event.set()
        except socket.timeout:
            continue
        except (OSError, ValueError, UnicodeError) as exc:
            log(f"Discovery warning: {exc}", "warning")
    sock.close()


def mark_stations_offline():
    with db_lock, db() as con:
        con.execute("UPDATE stations SET online=0")


def sync_station(station):
    sid, ip = station["station_id"], station["ip"]
    if not ip:
        return False
    base = f"http://{ip}"
    try:
        status = url_json(base + "/status", 5)
        status["station_id"] = status.get("station_id") or sid
        reported_role = status.get("sensor_role")
        if CAMERA_ONLY_MODE and reported_role in {"soil", "dht22"}:
            station_no = int(status.get("station_no") or sid[-1])
            query = urllib.parse.urlencode({"station": station_no, "role": "none"})
            url_json(base + "/device/config?" + query, 10)
            try:
                urllib.request.urlopen(base + "/reboot?confirm=YES", timeout=3).read()
            except (OSError, urllib.error.URLError):
                pass
            log(f"{sid} switched to Camera-only; waiting for restart")
            status["sensor_role"] = "camera"
        elif reported_role == "none":
            status["sensor_role"] = "camera"
        upsert_station(status, ip)
        with db_lock, db() as con:
            con.execute(
                "UPDATE stations SET status_json=?,online=1,last_seen=?,role=COALESCE(NULLIF(?,''),role),name=COALESCE(NULLIF(?,''),name) WHERE station_id=?",
                (
                    json.dumps(status),
                    now_iso(),
                    status.get("sensor_role", ""),
                    status.get("station_name", ""),
                    sid,
                ),
            )
        dates = url_json(base + "/dates", 8).get("dates", [])
        for date in dates:
            if not CAMERA_ONLY_MODE:
                sync_sensor_date(sid, base, date)
            sync_photo_date(sid, base, date)
            if not CAMERA_ONLY_MODE:
                associate_sensor_readings(sid, date)
        return True
    except Exception as exc:
        with db_lock, db() as con:
            con.execute("UPDATE stations SET online=0 WHERE station_id=?", (sid,))
        log(f"{sid} unavailable at {ip}: {exc}", "warning")
        return False


def sync_sensor_date(sid, base, date):
    try:
        data = url_json(
            base
            + "/sensors/list?"
            + urllib.parse.urlencode({"date": date, "page": 0, "per": 2000}),
            10,
        )
    except Exception:
        try:
            data = url_json(
                base
                + "/dht/list?"
                + urllib.parse.urlencode({"date": date, "page": 0, "per": 2000}),
                10,
            )
        except Exception:
            return
    with db_lock, db() as con:
        for row in data.get("rows", []):
            role = row.get("role") or ("soil" if "soil_percent" in row else "dht22")
            con.execute(
                """INSERT OR IGNORE INTO sensor_readings
                (station_id,date,reading_time,role,temperature_c,humidity_percent,soil_volts,soil_percent)
                VALUES(?,?,?,?,?,?,?,?)""",
                (
                    sid,
                    date,
                    row.get("t", ""),
                    role,
                    row.get("temperature_c", row.get("c")),
                    row.get("humidity_percent", row.get("h")),
                    row.get("volts"),
                    row.get("soil_percent"),
                ),
            )


def sync_photo_date(sid, base, date):
    page = 0
    while True:
        data = url_json(
            base + "/photos?" + urllib.parse.urlencode({"date": date, "page": page, "per": 500}), 15
        )
        files = data.get("files", [])
        stamp = now_iso()
        with db_lock, db() as con:
            for item in files:
                con.execute(
                    """INSERT OR IGNORE INTO photos
                  (station_id,date,filename,size,state,analyze,created_at,updated_at) VALUES(?,?,?,?,?,?,?,?)""",
                    (
                        sid,
                        date,
                        item.get("n"),
                        int(item.get("s") or 0),
                        "discovered",
                        1 if photo_is_new(date, item.get("n")) else 0,
                        stamp,
                        stamp,
                    ),
                )
        if page * 500 + len(files) >= int(data.get("total") or 0) or not files:
            break
        page += 1


def photo_is_new(date, filename):
    cutoff_text = get_setting("analysis_cutoff", "")
    if not cutoff_text:
        return True
    match = re.search(r"__(\d{2})-(\d{2})-(\d{2})", filename or "")
    if not match:
        return False
    try:
        cutoff = datetime.fromisoformat(cutoff_text)
        captured = datetime.strptime(
            f"{date} {match.group(1)}:{match.group(2)}:{match.group(3)}", "%Y-%m-%d %H:%M:%S"
        )
        captured = captured.replace(tzinfo=cutoff.tzinfo)
        return captured > cutoff
    except ValueError:
        return False


def associate_sensor_readings(sid, date):
    with db_lock, db() as con:
        readings = con.execute(
            "SELECT id,reading_time FROM sensor_readings WHERE station_id=? AND date=?", (sid, date)
        ).fetchall()
        if not readings:
            return
        timed = []
        for reading in readings:
            digits = re.sub(r"\D", "", reading["reading_time"] or "")[:6]
            if len(digits) == 6:
                timed.append(
                    (
                        reading["id"],
                        int(digits[:2]) * 3600 + int(digits[2:4]) * 60 + int(digits[4:]),
                    )
                )
        if not timed:
            return
        photos = con.execute(
            "SELECT filename FROM photos WHERE station_id=? AND date=?", (sid, date)
        ).fetchall()
        for photo in photos:
            match = re.search(r"__(\d{2})-(\d{2})-(\d{2})", photo["filename"])
            if not match:
                continue
            seconds = int(match.group(1)) * 3600 + int(match.group(2)) * 60 + int(match.group(3))
            reading_id = min(timed, key=lambda item: abs(item[1] - seconds))[0]
            con.execute(
                "UPDATE photos SET sensor_reading_id=? WHERE station_id=? AND date=? AND filename=?",
                (reading_id, sid, date, photo["filename"]),
            )


def valid_jpeg(path, expected=0):
    try:
        size = path.stat().st_size
        if size < 1000 or (expected and size != expected):
            return False
        with path.open("rb") as f:
            if f.read(2) != b"\xff\xd8":
                return False
            f.seek(-2, os.SEEK_END)
            return f.read(2) == b"\xff\xd9"
    except OSError:
        return False


def download_one(row, station_ip):
    sid, date, filename = row["station_id"], row["date"], row["filename"]
    folder = PHOTO_DIR / sid / date
    folder.mkdir(parents=True, exist_ok=True)
    final = folder / filename
    partial = final.with_suffix(final.suffix + ".part")
    if valid_jpeg(final, row["size"]):
        state = (
            "completed"
            if final.with_suffix(final.suffix + ".analysis.json").exists()
            else ("downloaded" if row["analyze"] else "archived")
        )
        with db_lock, db() as con:
            con.execute(
                "UPDATE photos SET state=?,local_path=?,updated_at=? WHERE station_id=? AND date=? AND filename=?",
                (state, str(final), now_iso(), sid, date, filename),
            )
        return True
    url = f"http://{station_ip}/photo?" + urllib.parse.urlencode(
        {"date": date, "name": filename, "dl": 1}
    )
    with db_lock, db() as con:
        con.execute(
            "UPDATE photos SET state='downloading',updated_at=? WHERE station_id=? AND date=? AND filename=?",
            (now_iso(), sid, date, filename),
        )
    try:
        with urllib.request.urlopen(url, timeout=45) as response, partial.open("wb") as out:
            shutil.copyfileobj(response, out, 64 * 1024)
        if not valid_jpeg(partial, row["size"]):
            raise ValueError("downloaded JPEG failed validation")
        os.replace(partial, final)
        with db_lock, db() as con:
            state = "downloaded" if row["analyze"] else "archived"
            con.execute(
                "UPDATE photos SET state=?,local_path=?,error=NULL,updated_at=? WHERE station_id=? AND date=? AND filename=?",
                (state, str(final), now_iso(), sid, date, filename),
            )
        return True
    except Exception as exc:
        partial.unlink(missing_ok=True)
        attempts = int(row["attempts"] or 0) + 1
        next_state = "failed" if attempts >= 5 else "discovered"
        with db_lock, db() as con:
            con.execute(
                "UPDATE photos SET state=?,attempts=?,error=?,updated_at=? WHERE station_id=? AND date=? AND filename=?",
                (next_state, attempts, str(exc)[:500], now_iso(), sid, date, filename),
            )
        return False


def download_round_robin():
    while not stop_event.is_set():
        made_progress = False
        with db_lock, db() as con:
            stations = con.execute(
                "SELECT station_id,ip FROM stations WHERE online=1 ORDER BY station_no"
            ).fetchall()
        for station in stations:
            with db_lock, db() as con:
                row = con.execute(
                    "SELECT * FROM photos WHERE station_id=? AND state='discovered' ORDER BY date,filename LIMIT 1",
                    (station["station_id"],),
                ).fetchone()
            if row:
                made_progress = True
                download_one(row, station["ip"])
        if not made_progress:
            return


def sync_worker():
    time.sleep(1)
    while not stop_event.is_set():
        sync_event.wait(60)
        sync_event.clear()
        try:
            with db_lock, db() as con:
                stations = con.execute("SELECT * FROM stations ORDER BY station_no").fetchall()
            for station in stations:
                sync_station(station)
            download_round_robin()
        except Exception as exc:
            log(f"Synchronization error: {exc}", "error")


def analysis_prompt(passes):
    if passes == 1:
        reconciliation = "Report that single visible count and its confidence."
    elif passes == 2:
        reconciliation = "If the two counts disagree, choose the better-supported visible count, reduce confidence, and explain the difference."
    else:
        reconciliation = "Use the majority result; if all three differ, choose the best-supported visible count and reduce confidence further."
    return (
        "Analyze this rice-plant photograph. Measure water level in centimeters only when a graduated metric scale and visible waterline can be read. "
        f"Perform exactly {passes} independent visual tiller-counting pass{'es' if passes != 1 else ''}; never perform an additional hidden pass. "
        f"Return exactly {passes} values in tiller_check_counts and put the selected result in final_tiller_count. Count stems emerging from the base, not leaves. {reconciliation} "
        "Use null when evidence is unreadable. Classify growth from visible morphology: seedling only when no distinct secondary basal tillers exist; "
        "active tillering (vegetative) when secondary basal shoots exist without reproductive structures; later stages only when defining structures such as booting or panicles are visible. "
        "If multiple plants are present, report the dominant stage and mention mixed stages. Explain visible evidence and any count disagreement in notes."
    )


def analysis_schema(passes):
    return {
        "type": "object",
        "additionalProperties": False,
        "properties": {
            "water_level_cm": {"type": ["number", "null"], "minimum": 0},
            "final_tiller_count": {"type": ["integer", "null"], "minimum": 0},
            "tiller_check_counts": {
                "type": "array",
                "items": {"type": ["integer", "null"], "minimum": 0},
                "minItems": passes,
                "maxItems": passes,
            },
            "growth_stage": {"type": ["string", "null"]},
            "water_confidence": {"type": "number", "minimum": 0, "maximum": 1},
            "tiller_confidence": {"type": "number", "minimum": 0, "maximum": 1},
            "growth_confidence": {"type": "number", "minimum": 0, "maximum": 1},
            "notes": {"type": "string"},
        },
        "required": [
            "water_level_cm",
            "final_tiller_count",
            "tiller_check_counts",
            "growth_stage",
            "water_confidence",
            "tiller_confidence",
            "growth_confidence",
            "notes",
        ],
    }


def openai_call(method, url, payload=None, timeout=120):
    headers = {"Authorization": f"Bearer {api_key()}", "Content-Type": "application/json"}
    body = json.dumps(payload, separators=(",", ":")).encode() if payload is not None else None
    req = urllib.request.Request(url, data=body, method=method, headers=headers)
    try:
        with urllib.request.urlopen(req, timeout=timeout) as response:
            return json.loads(response.read().decode("utf-8"))
    except urllib.error.HTTPError as exc:
        detail = exc.read().decode("utf-8", "replace")
        raise RuntimeError(f"OpenAI HTTP {exc.code}: {detail[:700]}") from exc


def nearest_sensor_context(row):
    if CAMERA_ONLY_MODE:
        return "Camera-only mode is active. Ignore all sensor data and do not infer missing sensor values."
    with db_lock, db() as con:
        reading = (
            con.execute(
                "SELECT * FROM sensor_readings WHERE id=?", (row["sensor_reading_id"],)
            ).fetchone()
            if row["sensor_reading_id"]
            else None
        )
        if not reading:
            reading = con.execute(
                """SELECT * FROM sensor_readings WHERE station_id=?
          ORDER BY ABS(julianday(date || ' ' || substr(reading_time,1,2) || ':' || substr(reading_time,3,2) || ':' || substr(reading_time,5,2))
          - julianday(? || ' 12:00:00')) LIMIT 1""",
                (row["station_id"], row["date"]),
            ).fetchone()
    if not reading:
        return "No synchronized sensor reading is available for this station. Do not infer a missing sensor value."
    fields = []
    for key, label, unit in (
        ("soil_volts", "soil sensor", "V"),
        ("soil_percent", "soil moisture", "%"),
        ("temperature_c", "temperature", "C"),
        ("humidity_percent", "humidity", "%"),
    ):
        if reading[key] is not None:
            fields.append(f"{label}={reading[key]:.2f}{unit}")
    if not fields:
        return "The nearest synchronized record contains no usable sensor fields. Do not treat missing fields as zero."
    return (
        f"Nearest sensor record ({reading['date']} {reading['reading_time']}): "
        + ", ".join(fields)
        + ". Use it only as supporting context."
    )


def submit_analysis(row):
    key = api_key()
    if not key:
        return False
    model = get_setting("model", "gpt-5.6-sol")
    effort = get_setting("effort", "low")
    passes = int(get_setting("passes", "1"))
    image_path = Path(row["local_path"])
    image = base64.b64encode(image_path.read_bytes()).decode("ascii")
    mime = mimetypes.guess_type(image_path.name)[0] or "image/jpeg"
    payload = {
        "model": model,
        "store": True,
        "background": True,
        "max_output_tokens": 8000,
        "reasoning": {"effort": effort},
        "metadata": {
            "station": row["station_id"],
            "date": row["date"],
            "filename": row["filename"][:64],
        },
        "input": [
            {
                "role": "user",
                "content": [
                    {
                        "type": "input_text",
                        "text": analysis_prompt(passes) + "\n\n" + nearest_sensor_context(row),
                    },
                    {
                        "type": "input_image",
                        "image_url": f"data:{mime};base64,{image}",
                        "detail": "high",
                    },
                ],
            }
        ],
        "text": {
            "format": {
                "type": "json_schema",
                "name": "rice_plant_analysis",
                "strict": True,
                "schema": analysis_schema(passes),
            }
        },
    }
    response = openai_call("POST", OPENAI_URL, payload)
    response_id = response.get("id")
    if not response_id:
        raise RuntimeError("OpenAI did not return a response ID")
    with db_lock, db() as con:
        con.execute(
            """UPDATE photos SET state='submitted',response_id=?,model=?,effort=?,passes=?,error=NULL,updated_at=?
          WHERE station_id=? AND date=? AND filename=?""",
            (
                response_id,
                model,
                effort,
                passes,
                now_iso(),
                row["station_id"],
                row["date"],
                row["filename"],
            ),
        )
    if response.get("status") == "completed":
        complete_analysis(row, response, model, effort, passes)
    return True


def extract_result(response):
    refusal = None
    text = None
    for item in response.get("output", []):
        if item.get("type") != "message":
            continue
        for part in item.get("content", []):
            if part.get("type") == "refusal":
                refusal = part.get("refusal")
            elif part.get("type") == "output_text":
                text = part.get("text")
    if refusal:
        raise RuntimeError(refusal)
    if not text:
        raise RuntimeError("OpenAI returned no structured result")
    return json.loads(text)


def complete_analysis(row, response, model=None, effort=None, passes=None):
    result = extract_result(response)
    model = model or row["model"] or response.get("model")
    effort = effort or row["effort"] or "low"
    passes = int(passes or row["passes"] or len(result.get("tiller_check_counts", [])))
    counts = result.get("tiller_check_counts", [])
    if len(counts) != passes:
        raise RuntimeError("OpenAI result did not contain the selected number of tiller passes")
    visible_counts = [value for value in counts if value is not None]
    if passes == 1:
        result["final_tiller_count"] = counts[0]
    elif passes == 2 and len(visible_counts) == 2 and visible_counts[0] != visible_counts[1]:
        result["tiller_confidence"] = round(float(result.get("tiller_confidence") or 0) * 0.75, 3)
    elif passes == 3 and visible_counts:
        majority = next(
            (value for value in visible_counts if visible_counts.count(value) >= 2), None
        )
        if majority is not None:
            result["final_tiller_count"] = majority
        elif len(visible_counts) == 3:
            result["tiller_confidence"] = round(
                float(result.get("tiller_confidence") or 0) * 0.6, 3
            )
    usage = response.get("usage") or {}
    record = {
        "station_id": row["station_id"],
        "date": row["date"],
        "filename": row["filename"],
        "model": model,
        "reasoning_effort": effort,
        "pass_count": passes,
        "response_id": response.get("id") or row["response_id"],
        "completed_at": now_iso(),
        "result": result,
        "usage": usage,
    }
    sidecar = Path(row["local_path"]).with_suffix(Path(row["local_path"]).suffix + ".analysis.json")
    temp = sidecar.with_suffix(sidecar.suffix + ".part")
    temp.write_text(json.dumps(record, indent=2), encoding="utf-8")
    os.replace(temp, sidecar)
    with db_lock, db() as con:
        con.execute(
            """INSERT INTO analyses(station_id,date,filename,result_json,model,effort,passes,response_id,input_tokens,output_tokens,completed_at)
          VALUES(?,?,?,?,?,?,?,?,?,?,?) ON CONFLICT(station_id,date,filename) DO UPDATE SET
          result_json=excluded.result_json,model=excluded.model,effort=excluded.effort,passes=excluded.passes,response_id=excluded.response_id,
          input_tokens=excluded.input_tokens,output_tokens=excluded.output_tokens,completed_at=excluded.completed_at""",
            (
                row["station_id"],
                row["date"],
                row["filename"],
                json.dumps(result),
                model,
                effort,
                passes,
                record["response_id"],
                int(usage.get("input_tokens") or 0),
                int(usage.get("output_tokens") or 0),
                record["completed_at"],
            ),
        )
        con.execute(
            "UPDATE photos SET state='completed',error=NULL,updated_at=? WHERE station_id=? AND date=? AND filename=?",
            (now_iso(), row["station_id"], row["date"], row["filename"]),
        )


def recover_submitted():
    with db_lock, db() as con:
        row = con.execute(
            "SELECT * FROM photos WHERE state='submitted' AND response_id IS NOT NULL ORDER BY updated_at LIMIT 1"
        ).fetchone()
    if not row:
        return False
    response = openai_call(
        "GET", OPENAI_URL + "/" + urllib.parse.quote(row["response_id"]), timeout=30
    )
    status = response.get("status")
    if status == "completed":
        complete_analysis(row, response)
    elif status in {"failed", "cancelled", "incomplete"}:
        detail = json.dumps(response.get("error") or response.get("incomplete_details") or status)
        with db_lock, db() as con:
            con.execute(
                "UPDATE photos SET state='failed',attempts=attempts+1,error=?,updated_at=? WHERE station_id=? AND date=? AND filename=?",
                (detail[:500], now_iso(), row["station_id"], row["date"], row["filename"]),
            )
    return True


def analysis_worker():
    station_cursor = 0
    while not stop_event.is_set():
        try:
            if not api_key():
                time.sleep(3)
                continue
            if recover_submitted():
                time.sleep(3)
                continue
            station_ids = ["plant-1", "plant-2", "plant-3"]
            row = None
            with db_lock, db() as con:
                for offset in range(3):
                    sid = station_ids[(station_cursor + offset) % 3]
                    row = con.execute(
                        "SELECT * FROM photos WHERE station_id=? AND state='downloaded' AND analyze=1 ORDER BY date,filename LIMIT 1",
                        (sid,),
                    ).fetchone()
                    if row:
                        station_cursor = (station_ids.index(sid) + 1) % 3
                        break
            if row:
                try:
                    submit_analysis(row)
                except Exception as exc:
                    attempts = int(row["attempts"] or 0) + 1
                    state = "failed" if attempts >= 3 else "downloaded"
                    with db_lock, db() as con:
                        con.execute(
                            "UPDATE photos SET state=?,attempts=?,error=?,updated_at=? WHERE station_id=? AND date=? AND filename=?",
                            (
                                state,
                                attempts,
                                str(exc)[:500],
                                now_iso(),
                                row["station_id"],
                                row["date"],
                                row["filename"],
                            ),
                        )
                    log(
                        f"Analysis failed for {row['station_id']}/{row['filename']}: {exc}", "error"
                    )
                    time.sleep(5)
            else:
                time.sleep(3)
        except Exception as exc:
            log(f"Analysis queue warning: {exc}", "warning")
            time.sleep(10)


def rescan_local_files():
    stamp = now_iso()
    station_dates = set()
    bad_sidecars = []
    with db_lock, db() as con:
        con.execute(
            "UPDATE photos SET state='discovered',response_id=NULL WHERE state='downloading'"
        )
        for sid_dir in PHOTO_DIR.glob("plant-*"):
            for date_dir in sid_dir.iterdir() if sid_dir.is_dir() else []:
                if not date_dir.is_dir():
                    continue
                for image in date_dir.glob("*.jpg"):
                    sidecar = image.with_suffix(image.suffix + ".analysis.json")
                    analyze = 1 if photo_is_new(date_dir.name, image.name) else 0
                    state = (
                        "completed"
                        if sidecar.exists()
                        else ("downloaded" if analyze else "archived")
                    )
                    con.execute(
                        """INSERT INTO photos(station_id,date,filename,size,state,local_path,analyze,created_at,updated_at)
                      VALUES(?,?,?,?,?,?,?,?,?) ON CONFLICT(station_id,date,filename) DO UPDATE SET
                      local_path=excluded.local_path,size=excluded.size,state=CASE WHEN photos.state='completed' THEN 'completed' ELSE excluded.state END""",
                        (
                            sid_dir.name,
                            date_dir.name,
                            image.name,
                            image.stat().st_size,
                            state,
                            str(image),
                            analyze,
                            stamp,
                            stamp,
                        ),
                    )
                    station_dates.add((sid_dir.name, date_dir.name))
                    if sidecar.exists():
                        try:
                            record = json.loads(sidecar.read_text(encoding="utf-8"))
                            result = record.get("result") or {}
                            usage = record.get("usage") or {}
                            con.execute(
                                """INSERT OR IGNORE INTO analyses
                              (station_id,date,filename,result_json,model,effort,passes,response_id,input_tokens,output_tokens,completed_at)
                              VALUES(?,?,?,?,?,?,?,?,?,?,?)""",
                                (
                                    sid_dir.name,
                                    date_dir.name,
                                    image.name,
                                    json.dumps(result),
                                    record.get("model") or "unknown",
                                    record.get("reasoning_effort") or "unknown",
                                    int(
                                        record.get("pass_count")
                                        or len(result.get("tiller_check_counts", []))
                                        or 1
                                    ),
                                    record.get("response_id"),
                                    int(usage.get("input_tokens") or 0),
                                    int(usage.get("output_tokens") or 0),
                                    record.get("completed_at") or stamp,
                                ),
                            )
                        except (OSError, ValueError, TypeError):
                            bad_sidecars.append(str(sidecar))
    for sid, date in station_dates:
        associate_sensor_readings(sid, date)
    for sidecar in bad_sidecars:
        log(f"Could not rebuild analysis from {sidecar}", "warning")


def dashboard_state():
    with db_lock, db() as con:
        stations = [dict(r) for r in con.execute("SELECT * FROM stations ORDER BY station_no")]
        for station in stations:
            try:
                station["status"] = json.loads(station.pop("status_json"))
            except ValueError:
                station["status"] = {}
            latest = con.execute(
                "SELECT date,filename,local_path,state FROM photos WHERE station_id=? AND local_path IS NOT NULL ORDER BY date DESC,filename DESC LIMIT 1",
                (station["station_id"],),
            ).fetchone()
            station["latest_photo"] = dict(latest) if latest else None
            if CAMERA_ONLY_MODE:
                station["latest_reading"] = None
                station["role"] = "camera"
                station["status"]["sensor_role"] = "camera"
            else:
                reading = con.execute(
                    "SELECT * FROM sensor_readings WHERE station_id=? ORDER BY date DESC,reading_time DESC LIMIT 1",
                    (station["station_id"],),
                ).fetchone()
                station["latest_reading"] = dict(reading) if reading else None
            analysis = con.execute(
                "SELECT * FROM analyses WHERE station_id=? ORDER BY date DESC,filename DESC LIMIT 1",
                (station["station_id"],),
            ).fetchone()
            if analysis:
                item = dict(analysis)
                item["result"] = json.loads(item.pop("result_json"))
                station["latest_analysis"] = item
            else:
                station["latest_analysis"] = None
        queue = {
            r["state"]: r["n"]
            for r in con.execute("SELECT state,count(*) n FROM photos GROUP BY state")
        }
        jobs = [
            dict(r)
            for r in con.execute(
                """SELECT p.station_id,p.date,p.filename,p.state,p.response_id,p.attempts,p.error,p.model,p.effort,p.passes,
          NULL AS reading_time,NULL AS soil_volts,NULL AS soil_percent,NULL AS temperature_c,NULL AS humidity_percent
          FROM photos p
          ORDER BY p.updated_at DESC LIMIT 100"""
            )
        ]
        events = [dict(r) for r in con.execute("SELECT * FROM event_log ORDER BY id DESC LIMIT 40")]
    disk = shutil.disk_usage(DATA_DIR)
    return {
        "stations": stations,
        "queue": queue,
        "jobs": jobs,
        "events": events,
        "settings": {
            "model": get_setting("model"),
            "effort": get_setting("effort"),
            "passes": int(get_setting("passes")),
            "api_key_set": bool(api_key()),
        },
        "storage": {"used": disk.used, "free": disk.free, "total": disk.total},
        "server_time": now_iso(),
        "port": HTTP_PORT,
    }


def configure_device(payload):
    ip = str(payload.get("ip", "")).strip()
    number = int(payload.get("station_no", 0))
    role = str(payload.get("role", "")).strip().lower()
    name = str(payload.get("name", f"Plant {number}")).strip()
    if not ip or number not in (1, 2, 3) or role not in ("soil", "dht22", "camera"):
        raise ValueError("IP, station number, and sensor profile are required")
    firmware_role = "none" if role == "camera" else role
    query = urllib.parse.urlencode({"station": number, "role": firmware_role, "name": name})
    result = url_json(f"http://{ip}/device/config?{query}", 10)
    sid = f"plant-{number}"
    upsert_station(
        {"station_id": sid, "station_no": number, "name": name, "role": role, "ip": ip}, ip
    )
    if payload.get("reboot", True):
        try:
            urllib.request.urlopen(f"http://{ip}/reboot?confirm=YES", timeout=3).read()
        except (OSError, urllib.error.URLError):
            pass
    sync_event.set()
    return result


def take_station_photo(payload):
    sid = str(payload.get("station_id", "")).strip()
    if sid not in {"plant-1", "plant-2", "plant-3"}:
        raise ValueError("A valid plant station is required")
    with db_lock, db() as con:
        station = con.execute("SELECT ip FROM stations WHERE station_id=?", (sid,)).fetchone()
    if not station or not station["ip"]:
        raise RuntimeError(f"{sid} has not been discovered on the network")
    result = url_json(f"http://{station['ip']}/snap", 20)
    if not result.get("ok"):
        raise RuntimeError(result.get("error") or f"{sid} could not take a photo")
    log(f"Manual photo requested from {sid}: {result.get('filename', 'saved')}")
    sync_event.set()
    return {"station_id": sid, "ip": station["ip"], **result}


class HubHTTPServer(ThreadingHTTPServer):
    # Windows SO_REUSEADDR can permit two processes to bind the same port.
    allow_reuse_address = False
    daemon_threads = True

    def server_bind(self):
        if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):
            self.socket.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
        super().server_bind()


class Handler(BaseHTTPRequestHandler):
    server_version = "RiceMonitoringHub/1.0"

    def send_json(self, obj, code=200):
        body = json.dumps(obj, separators=(",", ":"), default=str).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def read_json(self):
        length = int(self.headers.get("Content-Length", "0"))
        return json.loads(self.rfile.read(length) or b"{}")

    def do_GET(self):
        parsed = urllib.parse.urlparse(self.path)
        if parsed.path == "/":
            return self.send_file(STATIC_DIR / "index.html", "text/html; charset=utf-8")
        if parsed.path == "/api/state":
            return self.send_json(dashboard_state())
        if parsed.path == "/api/readings":
            if CAMERA_ONLY_MODE:
                return self.send_json({"rows": []})
            q = urllib.parse.parse_qs(parsed.query)
            sid = q.get("station_id", [""])[0]
            with db_lock, db() as con:
                rows = [
                    dict(r)
                    for r in con.execute(
                        "SELECT * FROM sensor_readings WHERE station_id=? ORDER BY date,reading_time LIMIT 5000",
                        (sid,),
                    )
                ]
            return self.send_json({"rows": rows})
        if parsed.path == "/api/analyses":
            with db_lock, db() as con:
                rows = [
                    dict(r)
                    for r in con.execute(
                        """SELECT a.*,NULL AS reading_time,NULL AS soil_volts,NULL AS soil_percent,NULL AS temperature_c,NULL AS humidity_percent
                  FROM analyses a JOIN photos p ON p.station_id=a.station_id AND p.date=a.date AND p.filename=a.filename
                  ORDER BY a.completed_at DESC LIMIT 300"""
                    )
                ]
            for r in rows:
                r["result"] = json.loads(r.pop("result_json"))
            return self.send_json({"rows": rows})
        if parsed.path == "/api/photo":
            q = urllib.parse.parse_qs(parsed.query)
            key = tuple(q.get(k, [""])[0] for k in ("station_id", "date", "filename"))
            with db_lock, db() as con:
                row = con.execute(
                    "SELECT local_path FROM photos WHERE station_id=? AND date=? AND filename=?",
                    key,
                ).fetchone()
            if row and row[0]:
                return self.send_file(Path(row[0]), "image/jpeg")
            return self.send_error(404)
        return self.send_error(404)

    def do_POST(self):
        parsed = urllib.parse.urlparse(self.path)
        try:
            payload = self.read_json()
            if parsed.path == "/api/settings":
                model, effort, passes = (
                    payload.get("model"),
                    payload.get("effort"),
                    int(payload.get("passes", 1)),
                )
                if model not in MODELS or effort not in EFFORTS or passes not in (1, 2, 3):
                    return self.send_json({"ok": False, "error": "invalid settings"}, 400)
                set_setting("model", model)
                set_setting("effort", effort)
                set_setting("passes", passes)
                if "api_key" in payload and payload["api_key"].strip():
                    save_api_key(payload["api_key"])
                return self.send_json({"ok": True})
            if parsed.path == "/api/sync":
                sync_event.set()
                return self.send_json({"ok": True})
            if parsed.path == "/api/snap":
                return self.send_json({"ok": True, "photo": take_station_photo(payload)})
            if parsed.path == "/api/stations":
                sid = payload.get("station_id")
                if sid not in {"plant-1", "plant-2", "plant-3"}:
                    return self.send_json({"ok": False}, 400)
                upsert_station(payload, payload.get("ip"))
                sync_event.set()
                return self.send_json({"ok": True})
            if parsed.path == "/api/device-config":
                return self.send_json({"ok": True, "device": configure_device(payload)})
            if parsed.path == "/api/reanalyze":
                key = tuple(payload.get(k, "") for k in ("station_id", "date", "filename"))
                with db_lock, db() as con:
                    con.execute(
                        """INSERT INTO analysis_history(station_id,date,filename,result_json,model,effort,passes,response_id,input_tokens,output_tokens,completed_at,archived_at)
                      SELECT station_id,date,filename,result_json,model,effort,passes,response_id,input_tokens,output_tokens,completed_at,?
                      FROM analyses WHERE station_id=? AND date=? AND filename=?""",
                        (now_iso(), *key),
                    )
                    con.execute(
                        "DELETE FROM analyses WHERE station_id=? AND date=? AND filename=?", key
                    )
                    con.execute(
                        "UPDATE photos SET state='downloaded',analyze=1,response_id=NULL,error=NULL,updated_at=? WHERE station_id=? AND date=? AND filename=?",
                        (now_iso(), *key),
                    )
                    image_row = con.execute(
                        "SELECT local_path FROM photos WHERE station_id=? AND date=? AND filename=?",
                        key,
                    ).fetchone()
                if image_row and image_row[0]:
                    sidecar = Path(image_row[0]).with_suffix(
                        Path(image_row[0]).suffix + ".analysis.json"
                    )
                    if sidecar.exists():
                        archived = sidecar.with_name(
                            sidecar.name.replace(
                                ".analysis.json",
                                f".analysis.{datetime.now().strftime('%Y%m%d-%H%M%S')}.json",
                            )
                        )
                        os.replace(sidecar, archived)
                return self.send_json({"ok": True})
            if parsed.path == "/api/retry":
                key = tuple(payload.get(k, "") for k in ("station_id", "date", "filename"))
                with db_lock, db() as con:
                    row = con.execute(
                        "SELECT local_path FROM photos WHERE station_id=? AND date=? AND filename=?",
                        key,
                    ).fetchone()
                    state = "downloaded" if row and row[0] else "discovered"
                    con.execute(
                        "UPDATE photos SET state=?,attempts=0,error=NULL,response_id=NULL,updated_at=? WHERE station_id=? AND date=? AND filename=?",
                        (state, now_iso(), *key),
                    )
                sync_event.set()
                return self.send_json({"ok": True})
        except Exception as exc:
            return self.send_json({"ok": False, "error": str(exc)}, 500)
        return self.send_error(404)

    def send_file(self, path, content_type):
        try:
            data = path.read_bytes()
        except OSError:
            return self.send_error(404)
        self.send_response(200)
        self.send_header("Content-Type", content_type)
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, fmt, *args):
        return


def run():
    # Reserve the TCP port before touching the database or starting workers.
    # A second launch opens the existing dashboard instead of another hub.
    try:
        server = HubHTTPServer(("0.0.0.0", HTTP_PORT), Handler)
    except OSError:
        try:
            url_json(f"http://127.0.0.1:{HTTP_PORT}/api/state", 3)
        except (OSError, ValueError) as exc:
            raise RuntimeError(f"Port {HTTP_PORT} is occupied by another service") from exc
        webbrowser.open(f"http://localhost:{HTTP_PORT}")
        return
    ensure_dirs()
    check_and_restore_database()
    initialize_database()
    rescan_local_files()
    make_backup()
    mark_stations_offline()
    threading.Thread(target=discovery_worker, name="discovery", daemon=True).start()
    threading.Thread(target=sync_worker, name="sync", daemon=True).start()
    threading.Thread(target=analysis_worker, name="analysis", daemon=True).start()
    threading.Thread(target=backup_worker, name="backup", daemon=True).start()
    sync_event.set()
    log(f"Monitoring hub ready on port {HTTP_PORT}")
    if "--open-browser" in sys.argv:
        webbrowser.open(f"http://localhost:{HTTP_PORT}")
    try:
        server.serve_forever(poll_interval=0.5)
    except KeyboardInterrupt:
        pass
    finally:
        stop_event.set()
        server.server_close()
        make_backup()


if __name__ == "__main__":
    run()
