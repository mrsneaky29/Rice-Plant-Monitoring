import importlib.util
import json
from pathlib import Path
import sys
from urllib.error import HTTPError


SCRIPT = Path(__file__).with_name("import_sd.py")
SPEC = importlib.util.spec_from_file_location("import_sd", SCRIPT)
import_sd = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = import_sd
SPEC.loader.exec_module(import_sd)


JPEG = b"\xff\xd8\xffsynthetic-camera-frame\xff\xd9"


def put_jpeg(path: Path) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(JPEG)


def configure_private_paths(monkeypatch, tmp_path):
    private = tmp_path / "Rice-Cloud" / ".private"
    monkeypatch.setattr(import_sd, "ROOT", private.parent)
    monkeypatch.setattr(import_sd, "PRIVATE", private)
    monkeypatch.setattr(import_sd, "MANIFEST_PATH", private / "sd-import-manifest.json")
    return private


def test_dry_run_preserves_card_and_records_date_unsorted_and_unknown_mapping(monkeypatch, tmp_path):
    private = configure_private_paths(monkeypatch, tmp_path)
    card = tmp_path / "sd-card"
    put_jpeg(card / "DCIM" / "2026-10-17" / "capture.jpg")
    put_jpeg(card / "DCIM" / "2024-02-29" / "leap-day.jpg")
    put_jpeg(card / "DCIM" / "2026-02-30" / "invalid-date.jpg")
    put_jpeg(card / "DCIM" / "UNSORTED" / "IMG_00001.jpg")
    put_jpeg(card / "DCIM" / "mystery-folder" / "old.jpg")
    put_jpeg(card / "DCIM" / "flat.jpg")
    put_jpeg(card / "DCIM" / "unsafe name.jpg")
    before = {p.relative_to(card).as_posix(): p.read_bytes() for p in card.rglob("*") if p.is_file()}

    summary = import_sd.run_import("plant-2", card, upload=False, config_path=private / "missing.json")

    after = {p.relative_to(card).as_posix(): p.read_bytes() for p in card.rglob("*") if p.is_file()}
    manifest = json.loads((private / "sd-import-manifest.json").read_text(encoding="utf-8"))
    assert before == after
    assert summary["dry_run"] == 6
    assert summary["skipped"] == 1
    assert summary["unknown_folder_mappings"] == 2
    by_source = {p["source"]: p for p in manifest["photos"]}
    assert by_source["2026-10-17/capture.jpg"]["date"] == "2026-10-17"
    assert by_source["2024-02-29/leap-day.jpg"]["date"] == "2024-02-29"
    assert by_source["2026-02-30/invalid-date.jpg"]["date"] == "UNSORTED"
    assert by_source["UNSORTED/IMG_00001.jpg"]["date"] == "UNSORTED"
    assert by_source["mystery-folder/old.jpg"]["date"] == "UNSORTED"
    assert by_source["flat.jpg"]["folder_mapping"] == "flat DCIM file has no date folder"
    assert {item["source_folder"] for item in manifest["unknown_folder_mappings"]} == {"2026-02-30", "mystery-folder"}
    assert private.is_dir()


def test_upload_uses_station_token_checksum_and_stops_at_409(monkeypatch, tmp_path):
    private = configure_private_paths(monkeypatch, tmp_path)
    card = tmp_path / "sd-card"
    put_jpeg(card / "DCIM" / "2026-10-17" / "a.jpg")
    put_jpeg(card / "DCIM" / "2026-10-17" / "b.jpg")
    config = tmp_path / "provision.json"
    config.write_text(json.dumps({"base_url": "https://example.test", "device_tokens": {"plant-1": "test-token"}}), encoding="utf-8")
    calls = []

    def conflict(request, **kwargs):
        calls.append(request)
        raise HTTPError(request.full_url, 409, "Conflict", {}, None)

    monkeypatch.setattr(import_sd, "urlopen", conflict)
    summary = import_sd.run_import("plant-1", card, upload=True, config_path=config)

    manifest = json.loads((private / "sd-import-manifest.json").read_text(encoding="utf-8"))
    assert len(calls) == 1
    assert calls[0].get_method() == "PUT"
    assert calls[0].full_url == "https://example.test/api/device/photos/2026-10-17/a.jpg"
    assert calls[0].get_header("Authorization") == "Bearer test-token"
    assert calls[0].get_header("X-content-sha256") == import_sd.hashlib.sha256(JPEG).hexdigest()
    assert summary["conflict_stopped"] is True
    assert manifest["photos"][0]["outcome"] == "http_409"
    assert manifest["photos"][1]["outcome"] == "pending"
    assert all("test-token" not in json.dumps(item) for item in manifest["photos"])


def test_upload_config_rejects_http_and_missing_station_token(tmp_path):
    config = tmp_path / "provision.json"
    config.write_text(json.dumps({"base_url": "http://example.test", "device_tokens": {"plant-1": "secret"}}), encoding="utf-8")
    try:
        import_sd.load_upload_config(config, "plant-1")
    except ValueError as exc:
        assert "HTTPS" in str(exc)
    else:
        raise AssertionError("HTTP upload endpoint must be rejected")

    config.write_text(json.dumps({"device_tokens": {"plant-1": "secret"}}), encoding="utf-8")
    try:
        import_sd.load_upload_config(config, "plant-2")
    except ValueError as exc:
        assert "plant-2" in str(exc)
    else:
        raise AssertionError("missing station token must be rejected")
