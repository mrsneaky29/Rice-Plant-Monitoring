import importlib.util
import http.client
import io
import json
from pathlib import Path
import sys
from urllib.parse import parse_qs, urlsplit


SCRIPT = Path(__file__).with_name("pull_camera.py")
SPEC = importlib.util.spec_from_file_location("pull_camera", SCRIPT)
pull_camera = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = pull_camera
SPEC.loader.exec_module(pull_camera)


class FakeResponse:
    def __init__(self, body):
        self.stream = io.BytesIO(body)

    def __enter__(self):
        return self

    def __exit__(self, *_args):
        return False

    def read(self, size=-1):
        return self.stream.read(size)


class BrokenResponse(FakeResponse):
    def __init__(self, body):
        super().__init__(body)
        self.failed = False

    def read(self, size=-1):
        if not self.failed:
            self.failed = True
            return self.stream.read(5)
        raise http.client.IncompleteRead(self.stream.read(), 1)


class Camera:
    def __init__(self, dates, photos, station="plant-2"):
        self.dates = dates
        self.photos = photos
        self.station = station
        self.calls = []

    def __call__(self, request, timeout):
        self.calls.append(request)
        parsed = urlsplit(request.full_url)
        query = parse_qs(parsed.query)
        if parsed.path == "/status":
            payload = {"station_id": self.station, "sd_mounted": True, "photo_count": sum(map(len, self.photos.values()))}
            return FakeResponse(json.dumps(payload).encode())
        if parsed.path == "/dates":
            return FakeResponse(json.dumps({"dates": self.dates}).encode())
        if parsed.path == "/photos":
            folder = query["date"][0]
            page, per = int(query["page"][0]), int(query["per"][0])
            files = self.photos[folder]
            start = page * per
            listing = [{"n": item["n"], "s": item["s"]} for item in files]
            return FakeResponse(json.dumps({"date": folder, "total": len(files), "page": page, "per": per, "files": listing[start:start + per]}).encode())
        if parsed.path == "/photo":
            folder, name = query["date"][0], query["name"][0]
            return FakeResponse(self.photos[folder][next(i for i, item in enumerate(self.photos[folder]) if item["n"] == name)]["body"])
        raise AssertionError(f"unexpected camera path: {parsed.path}")


def photo(name, body):
    return {"n": name, "s": len(body), "body": body}


def test_pull_fetches_all_pages_and_preserves_date_and_unsorted_names(tmp_path):
    body = b"\xff\xd8\xffplant-photo\xff\xd9"
    photos = {
        "2026-10-17": [photo("capture-1.jpg", body), photo("capture-2.jpg", body)],
        "UNSORTED": [photo("IMG_00001.jpg", body)],
    }
    camera = Camera(["2026-10-17", "UNSORTED"], photos)
    output = tmp_path / "Rice-Cloud" / ".private" / "sd-import" / "plant-2" / "DCIM"

    summary = pull_camera.pull("plant-2", "192.168.1.204", output, opener=camera)

    assert summary["source_photo_count"] == summary["enumerated_count"] == summary["downloaded_count"] == 3
    assert summary["complete"] is True
    assert (output / "2026-10-17" / "capture-1.jpg").read_bytes() == body
    assert (output / "UNSORTED" / "IMG_00001.jpg").read_bytes() == body
    assert all(call.get_method() == "GET" for call in camera.calls)
    manifest = json.loads((output.parent / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["complete"] is True
    assert len(manifest["photos"]) == 3


def test_pull_refuses_wrong_station_before_creating_output(tmp_path):
    camera = Camera([], {}, station="plant-1")
    output = tmp_path / "local" / "DCIM"
    try:
        pull_camera.pull("plant-2", "192.168.1.204", output, opener=camera)
    except pull_camera.PullError as exc:
        assert "identity" in str(exc)
    else:
        raise AssertionError("wrong camera identity must stop the pull")
    assert not output.exists()
    assert [urlsplit(call.full_url).path for call in camera.calls] == ["/status"]


def test_camera_get_retries_are_bounded_with_one_second_backoff(monkeypatch, tmp_path):
    camera = Camera([], {})
    requests = []
    waits = []

    def flaky(request, timeout):
        requests.append(request)
        if len(requests) < 3:
            raise TimeoutError("synthetic timeout")
        return camera(request, timeout)

    monkeypatch.setattr(pull_camera.time, "sleep", waits.append)
    output = tmp_path / "local" / "DCIM"
    summary = pull_camera.pull("plant-2", "192.168.1.204", output, opener=flaky)
    assert summary["complete"] is True
    assert len(requests) == 4
    assert [urlsplit(request.full_url).path for request in requests] == ["/status", "/status", "/status", "/dates"]
    assert waits == [1, 1]


def test_partial_photo_stream_retries_without_committing_partial_bytes(monkeypatch, tmp_path):
    body = b"\xff\xd8\xffcomplete-frame\xff\xd9"
    camera = Camera(["2026-10-17"], {"2026-10-17": [photo("capture.jpg", body)]})
    photo_requests = []
    waits = []

    def flaky_photo(request, timeout):
        if urlsplit(request.full_url).path == "/photo":
            photo_requests.append(request)
            if len(photo_requests) == 1:
                return BrokenResponse(body)
        return camera(request, timeout)

    monkeypatch.setattr(pull_camera.time, "sleep", waits.append)
    output = tmp_path / "local" / "DCIM"
    summary = pull_camera.pull("plant-2", "192.168.1.204", output, opener=flaky_photo)
    target = output / "2026-10-17" / "capture.jpg"
    assert summary["complete"] is True
    assert len(photo_requests) == 2
    assert waits == [1]
    assert target.read_bytes() == body
    assert list(target.parent.glob("*.part")) == []


def test_pull_keeps_existing_conflicting_file_and_resumes_matching_checksum(tmp_path):
    remote = b"\xff\xd8\xffremote-photo\xff\xd9"
    local_conflict = b"\xff\xd8\xffother-photo\xff\xd9"
    photos = {"2026-10-17": [photo("same.jpg", remote), photo("next.jpg", remote)]}
    camera = Camera(["2026-10-17"], photos)
    output = tmp_path / "local" / "DCIM"
    (output / "2026-10-17").mkdir(parents=True)
    (output / "2026-10-17" / "same.jpg").write_bytes(remote)
    (output / "2026-10-17" / "next.jpg").write_bytes(local_conflict)

    summary = pull_camera.pull("plant-2", "192.168.1.204", output, opener=camera)

    assert summary["already_present_count"] == 1
    assert summary["downloaded_count"] == 0
    assert summary["complete"] is False
    assert (output / "2026-10-17" / "same.jpg").read_bytes() == remote
    assert (output / "2026-10-17" / "next.jpg").read_bytes() == local_conflict
    manifest = json.loads((output.parent / "manifest.json").read_text(encoding="utf-8"))
    assert manifest["photos"][0]["outcome"] == "already_present"
    assert manifest["photos"][1]["outcome"] == "conflict_existing_local_file"
