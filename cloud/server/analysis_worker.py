"""Opt-in, durable photo analysis. Provider requests are never blindly resubmitted."""
from __future__ import annotations

import base64
import json
import os
import threading
import time
from urllib.parse import quote
import urllib.error
import urllib.request
from pathlib import Path
from typing import Any

from app import DATA_DIR, db

API_URL = os.environ.get("RICE_OPENAI_RESPONSES_URL", "https://api.openai.com/v1/responses").rstrip("/")
POLL_SECONDS = max(5, min(300, int(os.environ.get("RICE_ANALYSIS_POLL_SECONDS", "20"))))
_stop = threading.Event()
_thread: threading.Thread | None = None


def analysis_prompt(passes: int) -> str:
    if passes == 1:
        reconcile = "Report the single visible count and its confidence."
    elif passes == 2:
        reconcile = "If counts differ, select the better-supported visible count, reduce confidence, and explain the difference."
    else:
        reconcile = "Use the majority result; if all three differ, choose the best-supported visible count and reduce confidence further."
    return (
        "Analyze this rice-plant photograph. Measure water level in centimeters only when a graduated metric scale and visible waterline can be read. "
        f"Perform exactly {passes} visual tiller-counting verification passes within this evaluation and return exactly {passes} values in tiller_check_counts. "
        "Count stems emerging from the base, not leaves. Use null when evidence is unreadable. "
        "Classify growth from visible morphology: seedling only when no distinct secondary basal tillers exist; active tillering when secondary basal shoots exist without reproductive structures; later stages only when defining structures such as booting or panicles are visible. "
        "If multiple plants are present, report the dominant stage and mention mixed stages. Explain visible evidence and count disagreements in notes. " + reconcile
    )


def analysis_schema(passes: int) -> dict[str, Any]:
    return {
        "type": "object", "additionalProperties": False,
        "properties": {
            "water_level_cm": {"type": ["number", "null"], "minimum": 0},
            "final_tiller_count": {"type": ["integer", "null"], "minimum": 0},
            "tiller_check_counts": {"type": "array", "items": {"type": ["integer", "null"], "minimum": 0}, "minItems": passes, "maxItems": passes},
            "growth_stage": {"type": ["string", "null"]},
            "water_confidence": {"type": "number", "minimum": 0, "maximum": 1},
            "tiller_confidence": {"type": "number", "minimum": 0, "maximum": 1},
            "growth_confidence": {"type": "number", "minimum": 0, "maximum": 1},
            "notes": {"type": "string"},
        },
        "required": ["water_level_cm", "final_tiller_count", "tiller_check_counts", "growth_stage", "water_confidence", "tiller_confidence", "growth_confidence", "notes"],
    }


def _mark_uncertain(job_id: int, detail: str) -> None:
    with db() as conn:
        conn.execute("UPDATE analysis_jobs SET state='uncertain',error=?,updated_at=unixepoch() WHERE id=? AND state='submitting'", (detail[:500], job_id))


def stale_submissions_to_uncertain() -> None:
    with db() as conn:
        conn.execute("UPDATE analysis_jobs SET state='uncertain',error='Server restarted while provider submission was in progress; submission may have been accepted. It was not resent.',updated_at=unixepoch() WHERE state='submitting' AND response_id IS NULL")


def _http(method: str, url: str, payload: dict | None = None, timeout: int = 120) -> dict:
    key = os.environ["RICE_OPENAI_API_KEY"]
    data = json.dumps(payload, separators=(",", ":")).encode("utf-8") if payload is not None else None
    request = urllib.request.Request(url, data=data, method=method, headers={"Authorization": f"Bearer {key}", "Content-Type": "application/json"})
    with urllib.request.urlopen(request, timeout=timeout) as response:
        return json.loads(response.read().decode("utf-8"))


def _claim_queued() -> dict | None:
    with db() as conn:
        conn.execute("BEGIN IMMEDIATE")
        row = conn.execute("SELECT j.*,p.station_id,p.date,p.filename,p.path FROM analysis_jobs j JOIN photos p ON p.id=j.photo_id WHERE j.state='queued' ORDER BY j.created_at LIMIT 1").fetchone()
        if row is None:
            return None
        conn.execute("UPDATE analysis_jobs SET state='submitting',error=NULL,updated_at=unixepoch() WHERE id=? AND state='queued'", (row["id"],))
        return dict(row)


def process_queued_once() -> bool:
    job = _claim_queued()
    if not job:
        return False
    try:
        image_path = (DATA_DIR / job["path"]).resolve()
        if DATA_DIR not in image_path.parents or not image_path.is_file():
            with db() as conn:
                conn.execute("UPDATE analysis_jobs SET state='failed',error='Stored photo is missing or outside the data directory.',updated_at=unixepoch() WHERE id=?", (job["id"],))
            return True
        encoded = base64.b64encode(image_path.read_bytes()).decode("ascii")
        payload = {
            "model": job["model"], "background": True, "store": True,
            "max_output_tokens": 8000,
            "metadata": {"photo_job_id": str(job["id"]), "station": job["station_id"], "date": job["date"]},
            "input": [{"role": "user", "content": [
                {"type": "input_text", "text": analysis_prompt(job["passes"])},
                {"type": "input_image", "image_url": f"data:image/jpeg;base64,{encoded}", "detail": "high"},
            ]}],
            "text": {"format": {"type": "json_schema", "name": "rice_plant_analysis", "strict": True, "schema": analysis_schema(job["passes"])}},
        }
        if job["model"].startswith("gpt-6"):
            payload["reasoning"] = {"effort": "low"}
        response = _http("POST", API_URL, payload)
        response_id = response.get("id")
        if not isinstance(response_id, str) or not response_id:
            _mark_uncertain(job["id"], "Provider accepted a response without returning a response ID; it was not resubmitted.")
            return True
        with db() as conn:
            conn.execute("UPDATE analysis_jobs SET state='submitted',response_id=?,updated_at=unixepoch() WHERE id=? AND state='submitting'", (response_id, job["id"]))
        _finish_response(job["id"], response)
    except urllib.error.HTTPError as exc:
        if 400 <= exc.code < 500:
            with db() as conn:
                conn.execute("UPDATE analysis_jobs SET state='failed',error=?,updated_at=unixepoch() WHERE id=? AND state='submitting'", (f"Provider rejected request (HTTP {exc.code}); check server credentials, quota and model configuration.", job["id"]))
        else:
            _mark_uncertain(job["id"], f"Provider returned HTTP {exc.code}; request outcome may be ambiguous and was not resent.")
    except Exception as exc:
        _mark_uncertain(job["id"], f"Provider submission outcome is ambiguous ({type(exc).__name__}); it was not resent.")
    return True


def _finish_response(job_id: int, response: dict) -> None:
    status = response.get("status")
    if status in {"queued", "in_progress", "running"}:
        return
    if status in {"failed", "cancelled", "incomplete"}:
        detail = json.dumps(response.get("error") or response.get("incomplete_details") or status)[:500]
        with db() as conn:
            conn.execute("UPDATE analysis_jobs SET state='failed',error=?,updated_at=unixepoch() WHERE id=?", (detail, job_id))
        return
    if status != "completed":
        return
    text = None
    for item in response.get("output", []):
        if item.get("type") != "message":
            continue
        for content in item.get("content", []):
            if content.get("type") == "output_text":
                text = content.get("text")
            elif content.get("type") == "refusal":
                raise ValueError("Analysis provider refused the request")
    try:
        result = json.loads(text) if text else None
        if not isinstance(result, dict):
            raise ValueError("Completed response did not contain structured analysis")
        with db() as conn:
            job = conn.execute("SELECT * FROM analysis_jobs WHERE id=?", (job_id,)).fetchone()
            if not job:
                return
            counts = result.get("tiller_check_counts")
            if not isinstance(counts, list) or len(counts) != job["passes"]:
                raise ValueError("Analysis returned an invalid tiller count list")
            usage = response.get("usage") or {}
            conn.execute("UPDATE analysis_jobs SET state='completed',result_json=?,usage_json=?,error=NULL,updated_at=unixepoch() WHERE id=?", (json.dumps(result), json.dumps(usage), job_id))
            conn.execute("INSERT INTO analysis_records(source_table,source_key,station_id,date,filename,record_json) SELECT 'cloud_analysis',?,p.station_id,p.date,p.filename,? FROM photos p JOIN analysis_jobs j ON j.photo_id=p.id WHERE j.id=? ON CONFLICT(source_table,source_key) DO UPDATE SET record_json=excluded.record_json", (response.get("id") or f"job-{job_id}", json.dumps({"result": result, "model": job["model"], "passes": job["passes"], "response_id": response.get("id"), "usage": usage, "completed_at": int(time.time())}), job_id))
    except Exception as exc:
        with db() as conn:
            conn.execute("UPDATE analysis_jobs SET state='failed',error=?,updated_at=unixepoch() WHERE id=?", (f"Could not validate completed provider result: {type(exc).__name__}"[:500], job_id))


def poll_submitted_once() -> bool:
    with db() as conn:
        row = conn.execute("SELECT id,response_id FROM analysis_jobs WHERE state='submitted' AND response_id IS NOT NULL AND updated_at<=unixepoch()-? ORDER BY updated_at LIMIT 1", (POLL_SECONDS,)).fetchone()
    if not row:
        return False
    try:
        response = _http("GET", API_URL + "/" + quote(row["response_id"], safe=""), timeout=30)
        _finish_response(row["id"], response)
        with db() as conn:
            conn.execute("UPDATE analysis_jobs SET updated_at=unixepoch() WHERE id=? AND state='submitted'", (row["id"],))
    except urllib.error.HTTPError as exc:
        if exc.code == 404:
            with db() as conn:
                conn.execute("UPDATE analysis_jobs SET state='uncertain',error='Provider no longer recognizes the saved response ID; no replacement request was sent.',updated_at=unixepoch() WHERE id=? AND state='submitted'", (row["id"],))
        elif 400 <= exc.code < 500 and exc.code != 429:
            with db() as conn:
                conn.execute("UPDATE analysis_jobs SET state='failed',error=?,updated_at=unixepoch() WHERE id=? AND state='submitted'", (f"Provider rejected response lookup ({exc.code})."[:500], row["id"]))
        else:
            with db() as conn:
                conn.execute("UPDATE analysis_jobs SET updated_at=unixepoch() WHERE id=? AND state='submitted'", (row["id"],))
    except Exception:
        with db() as conn:
            conn.execute("UPDATE analysis_jobs SET updated_at=unixepoch() WHERE id=? AND state='submitted'", (row["id"],))
    return True


def _run() -> None:
    while not _stop.is_set():
        try:
            worked = poll_submitted_once() or process_queued_once()
        except Exception:
            worked = False
        _stop.wait(1 if worked else POLL_SECONDS)


def start_worker() -> None:
    global _thread
    _stop.clear()
    _thread = threading.Thread(target=_run, name="rice-analysis-worker", daemon=True)
    _thread.start()


def stop_worker() -> None:
    _stop.set()
    if _thread and _thread.is_alive():
        _thread.join(timeout=5)
