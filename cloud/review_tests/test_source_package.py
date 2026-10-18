"""Publication must exclude private files and leftover synthetic test storage."""
import importlib.util
from pathlib import Path


def test_package_publishes_sources_without_private_or_generated_storage(tmp_path):
    path = Path(__file__).resolve().parents[1] / 'deploy/package.py'
    spec = importlib.util.spec_from_file_location('source_package_under_test', path)
    package = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(package)
    package.ROOT = tmp_path
    allowed = {'README.md', '.gitignore', 'server/app.py', 'server/static/app.js', 'deploy/build_firmware.py',
               'firmware/ESP32_Station/cloud_run_mode.h', 'review_tests/test_example.py',
               'review_tests/policy_test.cpp'}
    forbidden = {'.private/server.env', 'firmware/ESP32_Station/secrets.h',
                 'firmware/ESP32_Station/cloud_secrets.h', 'server/data/photo.jpg',
                 'server/photo.jpeg', 'server/private.key', 'server/private.pem',
                 'server/monitor.sqlite3-wal', 'server/monitor.db',
                 'firmware/ESP32_Station/build/image.bin', 'backup.zip',
                 'AGY_RESTORE_HANDOFF.md', 'PHOTO_INVENTORY.md', 'AGY_REVIEW.md',
                 'review_tests/server_temp_review/test_0/fixture.json',
                 'review_tests/server_temp_review/test_0/leaked_fixture.py',
                 'unexpected/secrets.txt'}
    for relative in allowed | forbidden:
        p = tmp_path / relative
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text('synthetic content')
    assert {r.as_posix() for _, r in package.sources()} == allowed
    assert {r.as_posix() for _, r in package.sources(server_only=True)} == {
        'server/app.py', 'server/static/app.js', 'deploy/build_firmware.py'}
