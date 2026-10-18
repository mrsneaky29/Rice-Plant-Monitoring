"""Provision device credentials, manage admin password hashes, and import hub data."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import secrets
import shutil
import sqlite3
import sys
import tempfile
from pathlib import Path

from app import DATA_DIR, SAFE_DATE, SAFE_FILENAME, STATIONS, db, initialize, token_hash


def password_hash() -> None:
    first = _getpass("Admin password: ")
    second = _getpass("Repeat password: ")
    if first != second:
        raise SystemExit("Passwords did not match")
    if len(first) < 12:
        raise SystemExit("Use an admin password with at least 12 characters")
    salt = secrets.token_bytes(16)
    digest = hashlib.pbkdf2_hmac("sha256", first.encode(), salt, 600_000)
    print("pbkdf2_sha256$600000$" + salt.hex() + "$" + digest.hex())


def _getpass(prompt: str) -> str:
    import getpass
    return getpass.getpass(prompt)


def provision(station_id: str) -> None:
    if station_id not in STATIONS:
        raise SystemExit("station must be plant-1, plant-2, or plant-3")
    initialize()
    token = "rcd_" + secrets.token_urlsafe(36)
    with db() as conn:
        conn.execute("INSERT INTO device_tokens(token_hash,station_id,created_at) VALUES(?,?,unixepoch())", (token_hash(token), station_id))
    print(f"Provisioned one token for {station_id}. Copy it directly to that device now; it cannot be recovered later. Token SHA-256 for later revocation: {token_hash(token)}\n{token}")


def revoke(token_hash_value: str) -> None:
    if len(token_hash_value) != 64 or any(ch not in "0123456789abcdef" for ch in token_hash_value.lower()):
        raise SystemExit("Provide the SHA-256 token hash shown in the credential inventory")
    with db() as conn:
        result = conn.execute("UPDATE device_tokens SET revoked_at=unixepoch() WHERE token_hash=? AND revoked_at IS NULL", (token_hash_value.lower(),))
    print("Token revoked." if result.rowcount else "No active token matched.")


def import_legacy(source_db: Path, source_photos: Path) -> None:
    """Copy legacy rows and available JPEG files. Never changes the source DB/files."""
    if not source_db.is_file():
        raise SystemExit(f"SQLite database not found: {source_db}")
    initialize()
    source = sqlite3.connect(source_db.resolve().as_uri() + "?mode=ro", uri=True)
    snapshot_temp = tempfile.TemporaryDirectory(prefix="rice-cloud-import-")
    snapshot_path = Path(snapshot_temp.name) / "legacy-snapshot.sqlite3"
    snapshot = sqlite3.connect(snapshot_path)
    source.backup(snapshot)
    snapshot.close()
    source.close()
    source = sqlite3.connect(snapshot_path.resolve().as_uri() + "?mode=ro", uri=True)
    source.row_factory = sqlite3.Row
    available_tables = {r[0] for r in source.execute("SELECT name FROM sqlite_master WHERE type='table'")}
    imported_photos = 0
    archived_rows = 0
    copied_files = 0
    with db() as target:
        for table in ("stations", "photos", "analyses", "analysis_history", "sensor_readings", "event_log", "settings"):
            if table not in available_tables:
                continue
            rows = source.execute(f'SELECT * FROM "{table}"').fetchall()
            for row in rows:
                record = dict(row)
                if table == "stations":
                    station_id = record.get("station_id")
                    if station_id in STATIONS:
                        status = record.get("status_json") or "{}"
                        try:
                            json.loads(status)
                        except (ValueError, TypeError):
                            status = json.dumps({"legacy_status": status})
                        target.execute("UPDATE stations SET name=COALESCE(NULLIF(?,''),name),last_seen=COALESCE(?,last_seen),status_json=? WHERE station_id=?", (record.get("name"), _epoch(record.get("last_seen")), status, station_id))
                elif table == "photos":
                    station_id = record.get("station_id")
                    date, filename = record.get("date"), record.get("filename")
                    if station_id in STATIONS and isinstance(date, str) and SAFE_DATE.fullmatch(date) and isinstance(filename, str) and SAFE_FILENAME.fullmatch(filename):
                        source_file = _resolve_legacy_photo(record.get("local_path"), source_photos, station_id, date, filename)
                        if source_file and source_file.is_file():
                            relative = Path("photos") / station_id / date / filename
                            destination = DATA_DIR / relative
                            destination.parent.mkdir(parents=True, exist_ok=True)
                            source_checksum = _file_sha256(source_file)
                            if destination.exists() and _file_sha256(destination) != source_checksum:
                                raise SystemExit(f"Import conflict: {relative} already contains different photo data")
                            existing_photo = target.execute("SELECT checksum FROM photos WHERE station_id=? AND date=? AND filename=?", (station_id, date, filename)).fetchone()
                            if existing_photo and existing_photo["checksum"] != source_checksum:
                                raise SystemExit(f"Import conflict: {station_id}/{date}/{filename} already has a different database checksum")
                            if not destination.exists():
                                shutil.copy2(source_file, destination)
                                copied_files += 1
                            target.execute("INSERT OR IGNORE INTO photos(station_id,date,filename,checksum,path,uploaded_at) VALUES(?,?,?,?,?,?)", (station_id, date, filename, source_checksum, str(relative), _epoch(record.get("updated_at")) or 0))
                        key = f"{station_id}|{date}|{filename}"
                        target.execute("INSERT OR IGNORE INTO legacy_records(source_table,source_key,record_json) VALUES(?,?,?)", (table, key, json.dumps(record, ensure_ascii=False, default=str)))
                        imported_photos += 1
                elif table in {"analyses", "analysis_history"}:
                    key = _legacy_key(record, table)
                    target.execute("INSERT OR IGNORE INTO analysis_records(source_table,source_key,station_id,date,filename,record_json) VALUES(?,?,?,?,?,?)", (table, key, record.get("station_id"), record.get("date"), record.get("filename"), json.dumps(record, ensure_ascii=False, default=str)))
                    archived_rows += 1
                else:
                    key = _legacy_key(record, table)
                    target.execute("INSERT OR IGNORE INTO legacy_records(source_table,source_key,record_json) VALUES(?,?,?)", (table, key, json.dumps(record, ensure_ascii=False, default=str)))
                    archived_rows += 1
    source.close()
    snapshot_temp.cleanup()
    print(f"Import complete: {imported_photos} photo records, {copied_files} photo files, {archived_rows} analysis and legacy records copied. Source data was opened read-only.")


def _legacy_key(record: dict, table: str) -> str:
    keys = ("id", "station_id", "date", "filename", "created_at", "completed_at", "archived_at", "key")
    parts = [str(record[k]) for k in keys if record.get(k) is not None]
    return "|".join([table, *parts]) or hashlib.sha256(json.dumps(record, sort_keys=True, default=str).encode()).hexdigest()


def _resolve_legacy_photo(local_path: object, root: Path, station: str, date: str, filename: str) -> Path | None:
    allowed_root = root.resolve()
    candidates = []
    if isinstance(local_path, str) and local_path:
        candidates.append(Path(local_path))
    candidates.extend([root / station / date / filename, root / date / filename])
    for candidate in candidates:
        try:
            resolved = candidate.resolve()
            if resolved.is_relative_to(allowed_root) and resolved.is_file() and resolved.suffix.lower() in {".jpg", ".jpeg"}:
                return resolved
        except OSError:
            pass
    return None


def _epoch(value: object) -> int | None:
    if value is None:
        return None
    try:
        return int(value)
    except (ValueError, TypeError):
        try:
            from datetime import datetime
            return int(datetime.fromisoformat(str(value).replace("Z", "+00:00")).timestamp())
        except (ValueError, TypeError, OverflowError):
            return None


def _file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def main() -> None:
    parser = argparse.ArgumentParser(prog="rice-cloud-admin")
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("hash-password", help="create the environment hash for the fixed admin password")
    issue = commands.add_parser("provision", help="create one station-bound device token")
    issue.add_argument("station_id", choices=sorted(STATIONS))
    rev = commands.add_parser("revoke", help="revoke a token using its SHA-256 hash")
    rev.add_argument("token_hash")
    imp = commands.add_parser("import-legacy", help="copy data from the former local hub without modifying its source")
    imp.add_argument("--db", required=True, type=Path)
    imp.add_argument("--photos", required=True, type=Path)
    args = parser.parse_args()
    if args.command == "hash-password":
        password_hash()
    elif args.command == "provision":
        provision(args.station_id)
    elif args.command == "revoke":
        revoke(args.token_hash)
    else:
        import_legacy(args.db, args.photos)


if __name__ == "__main__":
    main()
