#!/usr/bin/env python3
"""Read photos from one ESP32 camera SD card into a private local folder."""

from __future__ import annotations

import argparse
from datetime import date, datetime, timezone
import hashlib
import http.client
import ipaddress
import json
import os
from pathlib import Path
import re
import ssl
import sys
import tempfile
import time
from typing import Any, Callable
from urllib.error import HTTPError, URLError
from urllib.parse import urlencode
from urllib.request import Request, urlopen
import uuid


ROOT = Path(__file__).resolve().parents[1]
PRIVATE = ROOT / ".private"
STATIONS = ("plant-1", "plant-2", "plant-3")
PAGE_SIZE = 100
MAX_DATES = 500
MAX_PHOTOS = 5000
MAX_PHOTO_BYTES = 64 * 1024 * 1024
GET_RETRIES = 3
RETRY_BACKOFF_SECONDS = 1
SAFE_FILENAME = re.compile(r"^[A-Za-z0-9][A-Za-z0-9_.-]{0,119}\.jpe?g$", re.IGNORECASE)
SAFE_DATE = re.compile(r"^\d{4}-\d{2}-\d{2}$")


class PullError(RuntimeError):
    pass


def valid_date(value: str) -> bool:
    if not SAFE_DATE.fullmatch(value):
        return False
    try:
        return date.fromisoformat(value).isoformat() == value
    except ValueError:
        return False


def validate_jpeg(path: Path, size: int) -> bool:
    if size < 4 or size > MAX_PHOTO_BYTES:
        return False
    with path.open("rb") as source:
        start = source.read(3)
        source.seek(-2, os.SEEK_END)
        end = source.read(2)
    return start == b"\xff\xd8\xff" and end == b"\xff\xd9"


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class CameraClient:
    def __init__(self, host: str, timeout: float = 25, opener: Callable[..., Any] | None = None):
        try:
            self.host = str(ipaddress.IPv4Address(host))
        except ipaddress.AddressValueError as exc:
            raise ValueError("Camera host must be an IPv4 address") from exc
        self.base_url = f"http://{self.host}"
        self.timeout = timeout
        self.opener = opener or urlopen

    def _request(self, path: str, params: dict[str, Any] | None = None):
        url = self.base_url + path
        if params:
            url += "?" + urlencode(params)
        return self.opener(Request(url, headers={"Accept": "application/json, image/jpeg"}), timeout=self.timeout)

    def _with_get_retries(self, action: Callable[[], Any], failure_message: str) -> Any:
        for attempt in range(GET_RETRIES + 1):
            try:
                return action()
            except HTTPError as exc:
                raise PullError(f"Camera returned HTTP {exc.code}") from None
            except (URLError, TimeoutError, OSError, http.client.HTTPException):
                if attempt == GET_RETRIES:
                    raise PullError(failure_message) from None
                time.sleep(RETRY_BACKOFF_SECONDS)

    def get_json(self, path: str, params: dict[str, Any] | None = None) -> dict[str, Any]:
        def read_json():
            with self._request(path, params) as response:
                return json.loads(response.read().decode("utf-8"))
        try:
            payload = self._with_get_retries(read_json, "Camera request failed or timed out")
        except (UnicodeDecodeError, json.JSONDecodeError):
            raise PullError("Camera returned invalid JSON") from None
        if not isinstance(payload, dict):
            raise PullError("Camera returned an unexpected JSON response")
        return payload

    def status(self) -> dict[str, Any]:
        return self.get_json("/status")

    def enumerate_photos(self, expected_station: str) -> tuple[dict[str, Any], list[dict[str, Any]]]:
        status = self.status()
        if status.get("station_id") != expected_station:
            raise PullError("Camera station identity does not match the requested station")
        if status.get("sd_mounted") is not True:
            raise PullError("Camera SD card is not mounted")
        photo_count = status.get("photo_count")
        if not isinstance(photo_count, int) or isinstance(photo_count, bool) or not 0 <= photo_count <= MAX_PHOTOS:
            raise PullError("Camera reported an invalid photo count")

        dates_payload = self.get_json("/dates")
        dates = dates_payload.get("dates")
        if not isinstance(dates, list) or len(dates) > MAX_DATES or any(not isinstance(value, str) for value in dates):
            raise PullError("Camera returned an invalid date list")
        if len(set(dates)) != len(dates):
            raise PullError("Camera returned duplicate date folders")
        for folder in dates:
            if folder != "UNSORTED" and not valid_date(folder):
                raise PullError("Camera returned an unexpected date folder")

        photos: list[dict[str, Any]] = []
        identities: set[tuple[str, str]] = set()
        for folder in dates:
            expected_total: int | None = None
            page = 0
            while True:
                payload = self.get_json("/photos", {"date": folder, "page": page, "per": PAGE_SIZE})
                total = payload.get("total")
                returned_page = payload.get("page")
                returned_per = payload.get("per")
                files = payload.get("files")
                if (
                    not isinstance(total, int) or isinstance(total, bool) or total < 0
                    or returned_page != page or returned_per != PAGE_SIZE or not isinstance(files, list)
                ):
                    raise PullError("Camera returned invalid paginated photo data")
                if expected_total is None:
                    expected_total = total
                elif expected_total != total:
                    raise PullError("Photo count changed during enumeration; no download started")
                wanted = min(PAGE_SIZE, max(0, total - page * PAGE_SIZE))
                if len(files) != wanted:
                    raise PullError("Camera returned an incomplete photo page; no download started")
                for entry in files:
                    if not isinstance(entry, dict):
                        raise PullError("Camera returned an invalid photo entry")
                    filename, size = entry.get("n"), entry.get("s")
                    if not isinstance(filename, str) or not SAFE_FILENAME.fullmatch(filename):
                        raise PullError("Camera returned an unsafe photo filename")
                    if not isinstance(size, int) or isinstance(size, bool) or not 4 <= size <= MAX_PHOTO_BYTES:
                        raise PullError("Camera returned an invalid photo size")
                    identity = (folder, filename)
                    if identity in identities:
                        raise PullError("Camera returned a duplicate photo identity")
                    identities.add(identity)
                    photos.append({"date": folder, "filename": filename, "size": size})
                if len(photos) > MAX_PHOTOS:
                    raise PullError("Camera photo list exceeded the safety limit")
                if (page + 1) * PAGE_SIZE >= total:
                    break
                page += 1

        if len(photos) != photo_count:
            raise PullError("Camera photo_count does not match its date and page listings; no download started")
        return status, photos

    def stream_photo(self, folder: str, filename: str, destination: Path) -> tuple[int, str]:
        return self._with_get_retries(
            lambda: self._stream_photo_once(folder, filename, destination),
            "Photo download failed or timed out",
        )

    def _stream_photo_once(self, folder: str, filename: str, destination: Path) -> tuple[int, str]:
        query = {"date": folder, "name": filename}
        response = self._request("/photo", query)
        digest = hashlib.sha256()
        size = 0
        first = b""
        last = b""
        destination.parent.mkdir(parents=True, exist_ok=True)
        temp_path: Path | None = None
        try:
            with response:
                with tempfile.NamedTemporaryFile(mode="wb", dir=destination.parent, prefix="pull-", suffix=".part", delete=False) as output:
                    temp_path = Path(output.name)
                    while True:
                        chunk = response.read(64 * 1024)
                        if not chunk:
                            break
                        size += len(chunk)
                        if size > MAX_PHOTO_BYTES:
                            raise PullError("Photo exceeded the 64 MiB safety limit")
                        if len(first) < 3:
                            first += chunk[:3 - len(first)]
                        last = (last + chunk)[-2:]
                        digest.update(chunk)
                        output.write(chunk)
            if size < 4 or first != b"\xff\xd8\xff" or last != b"\xff\xd9":
                raise PullError("Camera returned an incomplete JPEG")
            if temp_path is None:
                raise PullError("Photo staging file was not created")
            try:
                if os.name == "nt":
                    # Windows rename fails atomically when the destination exists.
                    os.rename(temp_path, destination)
                else:
                    # On POSIX, link creation is atomic and never replaces a target.
                    os.link(temp_path, destination)
            except FileExistsError:
                raise PullError("A local file appeared at this photo identity during the pull") from None
            if os.name != "nt":
                temp_path.unlink()
            temp_path = None
            return size, digest.hexdigest()
        except PullError:
            raise
        finally:
            if temp_path is not None:
                try:
                    temp_path.unlink()
                except OSError:
                    pass


def write_manifest(path: Path, manifest: dict[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temp = path.with_name(f"manifest-{uuid.uuid4().hex}.part")
    try:
        temp.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
        os.replace(temp, path)
    finally:
        if temp.exists():
            temp.unlink()


def pull(station: str, host: str, output_dcim: Path, timeout: float = 25, opener: Callable[..., Any] | None = None) -> dict[str, Any]:
    client = CameraClient(host, timeout, opener)
    status, photos = client.enumerate_photos(station)
    root = output_dcim.expanduser().resolve()
    manifest_path = root.parent / "manifest.json"
    manifest: dict[str, Any] = {
        "station_id": station,
        "camera_host": client.host,
        "retrieved_at_utc": datetime.now(timezone.utc).isoformat(),
        "camera_photo_count": status["photo_count"],
        "sd_mounted": status["sd_mounted"],
        "photos": [],
        "complete": False,
    }
    root.mkdir(parents=True, exist_ok=True)
    existing_conflict: dict[str, str] | None = None
    downloaded = skipped = 0
    for photo in photos:
        target = root / photo["date"] / photo["filename"]
        record = {**photo, "local_path": target.relative_to(root).as_posix(), "outcome": "pending"}
        manifest["photos"].append(record)
        write_manifest(manifest_path, manifest)
        if target.exists():
            try:
                if target.is_file() and target.stat().st_size == photo["size"] and validate_jpeg(target, photo["size"]):
                    local_hash = sha256_file(target)
                    # Hash the remote stream to compare existing bytes without replacing a file on mismatch.
                    temp_target = target.with_name(f"compare-{uuid.uuid4().hex}.part")
                    remote_size, remote_hash = client.stream_photo(photo["date"], photo["filename"], temp_target)
                    try:
                        temp_target.unlink()
                    except OSError:
                        pass
                    if remote_size == photo["size"] and local_hash == remote_hash:
                        record["sha256"] = local_hash
                        record["outcome"] = "already_present"
                        skipped += 1
                        write_manifest(manifest_path, manifest)
                        continue
            except PullError as exc:
                record["outcome"] = "download_error"
                manifest["stopped_reason"] = str(exc)
                write_manifest(manifest_path, manifest)
                break
            record["outcome"] = "conflict_existing_local_file"
            existing_conflict = {"date": photo["date"], "filename": photo["filename"]}
            manifest["stopped_on_conflict"] = existing_conflict
            write_manifest(manifest_path, manifest)
            break

        try:
            size, digest = client.stream_photo(photo["date"], photo["filename"], target)
        except PullError as exc:
            record["outcome"] = "download_error"
            manifest["stopped_reason"] = str(exc)
            write_manifest(manifest_path, manifest)
            break
        if size != photo["size"]:
            try:
                target.unlink()
            except OSError:
                pass
            record["outcome"] = "conflict_source_size_changed"
            manifest["stopped_on_conflict"] = {"date": photo["date"], "filename": photo["filename"]}
            write_manifest(manifest_path, manifest)
            break
        record["sha256"] = digest
        record["outcome"] = "downloaded"
        downloaded += 1
        write_manifest(manifest_path, manifest)

    complete = existing_conflict is None and len(manifest["photos"]) == len(photos) and all(
        entry["outcome"] in {"downloaded", "already_present"} for entry in manifest["photos"]
    )
    manifest["complete"] = complete
    manifest["downloaded_count"] = downloaded
    manifest["already_present_count"] = skipped
    manifest["enumerated_count"] = len(photos)
    manifest["folders"] = sorted({photo["date"] for photo in photos})
    write_manifest(manifest_path, manifest)
    return {
        "station_id": station,
        "camera_host": client.host,
        "source_photo_count": status["photo_count"],
        "enumerated_count": len(photos),
        "downloaded_count": downloaded,
        "already_present_count": skipped,
        "complete": complete,
        "manifest": str(manifest_path),
        "output_dcim": str(root),
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("station", choices=STATIONS)
    parser.add_argument("host", help="the camera's current IPv4 address; no network discovery is performed")
    parser.add_argument("--output", type=Path, help="local DCIM directory (default: Rice-Cloud/.private/sd-import/<station>/DCIM)")
    parser.add_argument("--timeout", type=float, default=25)
    args = parser.parse_args(argv)
    output = args.output or (PRIVATE / "sd-import" / args.station / "DCIM")
    try:
        summary = pull(args.station, args.host, output, args.timeout)
    except (PullError, ValueError, OSError) as exc:
        parser.error(str(exc))
    print(json.dumps(summary, ensure_ascii=False))
    return 0 if summary["complete"] else 2


if __name__ == "__main__":
    sys.exit(main())
