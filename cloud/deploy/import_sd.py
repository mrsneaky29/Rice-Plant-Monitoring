#!/usr/bin/env python3
"""Inventory or upload camera JPEGs from an ESP32 SD card without changing it."""

from __future__ import annotations

import argparse
from datetime import date, datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import ssl
import sys
from typing import Any
from urllib.error import HTTPError, URLError
from urllib.parse import quote, urlsplit
from urllib.request import Request, urlopen


ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / ".private"
DEFAULT_CONFIG = PRIVATE / "provision.json"
MANIFEST_PATH = PRIVATE / "sd-import-manifest.json"
DEFAULT_BASE_URL = "https://admin.riceplant.achantalabs.com"
STATIONS = ("plant-1", "plant-2", "plant-3")
SAFE_FILENAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,119}\.jpe?g$", re.IGNORECASE)
SAFE_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")
MAX_PHOTO_BYTES = 16 * 1024 * 1024


def safe_date_folder(value: str) -> bool:
    if not SAFE_DATE.fullmatch(value):
        return False
    try:
        parsed = date.fromisoformat(value)
    except ValueError:
        return False
    return parsed.isoformat() == value


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def is_complete_jpeg(path: Path, size: int) -> bool:
    if size < 4 or size > MAX_PHOTO_BYTES:
        return False
    with path.open("rb") as source:
        head = source.read(3)
        source.seek(-2, 2)
        tail = source.read(2)
    return head == b"\xff\xd8\xff" and tail == b"\xff\xd9"


def scan_sd(station: str, sd_root: Path) -> dict[str, Any]:
    root = sd_root.expanduser().resolve()
    if not root.is_dir():
        raise ValueError("SD root must be an accessible directory")
    dcim = root / "DCIM"
    if not dcim.is_dir():
        if root.name.casefold() == "dcim":
            dcim = root
        else:
            raise ValueError("SD root must contain a DCIM directory")

    entries: list[dict[str, Any]] = []
    mappings: list[dict[str, str]] = []
    invalid: list[dict[str, str]] = []
    identity_hashes: dict[tuple[str, str], str] = {}
    files = sorted(dcim.rglob("*"), key=lambda item: item.as_posix().casefold())
    for path in files:
        if path.is_symlink() or not path.is_file() or path.suffix.casefold() not in {".jpg", ".jpeg"}:
            continue

        relative = path.relative_to(dcim)
        if len(relative.parts) == 1:
            folder = "DCIM"
            mapped_date = "UNSORTED"
            mapping_reason = "flat DCIM file has no date folder"
        else:
            folder = relative.parts[0]
            if folder == "UNSORTED":
                mapped_date, mapping_reason = "UNSORTED", "preserved UNSORTED folder"
            elif safe_date_folder(folder):
                mapped_date, mapping_reason = folder, "valid YYYY-MM-DD folder"
            else:
                mapped_date, mapping_reason = "UNSORTED", "unknown or invalid folder mapped to UNSORTED"
        if mapping_reason == "unknown or invalid folder mapped to UNSORTED":
            mappings.append({"source_folder": folder, "mapped_date": mapped_date, "reason": mapping_reason})

        filename = path.name
        if not SAFE_FILENAME.fullmatch(filename):
            invalid.append({"source": relative.as_posix(), "reason": "unsafe filename"})
            continue

        size = path.stat().st_size
        if not is_complete_jpeg(path, size):
            invalid.append({"source": relative.as_posix(), "reason": "incomplete JPEG or exceeds 16 MiB"})
            continue
        digest = sha256_file(path)
        identity = (mapped_date, filename)
        previous = identity_hashes.get(identity)
        if previous and previous != digest:
            invalid.append({"source": relative.as_posix(), "reason": "same station/date/filename maps to different photo data"})
            continue
        identity_hashes[identity] = digest
        entries.append({
            "station_id": station,
            "date": mapped_date,
            "filename": filename,
            "source": relative.as_posix(),
            "size": size,
            "sha256": digest,
            "folder_mapping": mapping_reason,
            "outcome": "pending",
        })

    return {
        "station_id": station,
        "sd_root": str(root),
        "dcim_root": str(dcim),
        "scanned_at": datetime.now(timezone.utc).isoformat(),
        "mode": "inventory",
        "photos": entries,
        "unknown_folder_mappings": mappings,
        "skipped": invalid,
        "stopped_on_conflict": False,
    }


def load_upload_config(path: Path, station: str) -> tuple[str, str]:
    try:
        config = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        raise ValueError("Could not read the private upload configuration") from exc
    tokens = config.get("device_tokens") if isinstance(config, dict) else None
    token = tokens.get(station) if isinstance(tokens, dict) else None
    if not isinstance(token, str) or not token.strip():
        raise ValueError(f"Private upload configuration has no token for {station}")
    base_url = config.get("base_url", DEFAULT_BASE_URL)
    if not isinstance(base_url, str):
        raise ValueError("Private upload configuration has an invalid base_url")
    parsed = urlsplit(base_url.rstrip("/"))
    if parsed.scheme != "https" or not parsed.netloc or parsed.username or parsed.password or parsed.query or parsed.fragment:
        raise ValueError("Upload server must be an HTTPS base URL without embedded credentials")
    return base_url.rstrip("/"), token


def upload_photo(base_url: str, station: str, token: str, photo: dict[str, Any], timeout: int = 30) -> str:
    source = Path(photo["_absolute_source"])
    url = (
        f"{base_url}/api/device/photos/{quote(photo['date'], safe='')}/"
        f"{quote(photo['filename'], safe='')}"
    )
    request = Request(
        url,
        data=source.read_bytes(),
        method="PUT",
        headers={
            "Authorization": f"Bearer {token}",
            "Content-Type": "image/jpeg",
            "X-Content-SHA256": photo["sha256"],
            "X-Rice-Archive-Import": "true",
        },
    )
    try:
        with urlopen(request, timeout=timeout, context=ssl.create_default_context()) as response:
            if response.status != 200:
                return f"http_{response.status}"
            body = json.loads(response.read().decode("utf-8"))
    except HTTPError as exc:
        return f"http_{exc.code}"
    except (URLError, TimeoutError, OSError, json.JSONDecodeError):
        return "network_or_response_error"
    if not isinstance(body, dict) or body.get("ok") is not True:
        return "unexpected_response"
    return "duplicate" if body.get("duplicate") is True else "uploaded"


def save_manifest(manifest: dict[str, Any]) -> None:
    PRIVATE.mkdir(parents=True, exist_ok=True)
    MANIFEST_PATH.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


def run_import(station: str, sd_root: Path, upload: bool, config_path: Path) -> dict[str, Any]:
    manifest = scan_sd(station, sd_root)
    base_url = token = None
    if upload:
        base_url, token = load_upload_config(config_path, station)
        manifest["mode"] = "upload"

    for photo in manifest["photos"]:
        if not upload:
            photo["outcome"] = "dry_run"
            continue
        photo["_absolute_source"] = str(Path(manifest["dcim_root"]) / Path(photo["source"]))
        outcome = upload_photo(base_url, station, token, photo)
        photo.pop("_absolute_source", None)
        photo["outcome"] = outcome
        if outcome == "http_409":
            manifest["stopped_on_conflict"] = True
            photo["detail"] = "Server reports this station/date/filename already exists with different data; later uploads were stopped."
            break
        if outcome not in {"uploaded", "duplicate"}:
            photo["detail"] = "Upload did not complete; later uploads were stopped."
            break

    save_manifest(manifest)
    summary = {
        "station_id": station,
        "mode": manifest["mode"],
        "photos_found": len(manifest["photos"]),
        "unknown_folder_mappings": len(manifest["unknown_folder_mappings"]),
        "skipped": len(manifest["skipped"]),
        "uploaded": sum(item["outcome"] == "uploaded" for item in manifest["photos"]),
        "server_duplicates": sum(item["outcome"] == "duplicate" for item in manifest["photos"]),
        "dry_run": sum(item["outcome"] == "dry_run" for item in manifest["photos"]),
        "conflict_stopped": manifest["stopped_on_conflict"],
        "manifest": str(MANIFEST_PATH),
    }
    return summary


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("station", choices=STATIONS, help="station identity used by the server")
    parser.add_argument("sdroot", type=Path, help="mounted SD card root, containing DCIM")
    parser.add_argument("--upload", action="store_true", help="upload after inventory; omitted means dry-run only")
    parser.add_argument("--config", type=Path, default=DEFAULT_CONFIG, help="private provisioning JSON (never printed)")
    args = parser.parse_args(argv)
    try:
        summary = run_import(args.station, args.sdroot, args.upload, args.config)
    except ValueError as exc:
        parser.error(str(exc))
    except OSError:
        parser.error("Could not read the SD card or write the private inventory manifest")
    print(json.dumps(summary, ensure_ascii=False))
    return 0 if not summary["conflict_stopped"] else 2


if __name__ == "__main__":
    sys.exit(main())
