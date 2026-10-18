import importlib.util
import hashlib
from pathlib import Path
import sqlite3
import tarfile

import pytest

spec = importlib.util.spec_from_file_location('rice_backup', Path(__file__).resolve().parents[2] / 'deploy' / 'backup.py')
backup = importlib.util.module_from_spec(spec)
spec.loader.exec_module(backup)


def storage(tmp_path):
    data = tmp_path / 'data'
    data.mkdir()
    photo = data / 'photos' / 'plant-1' / '2026-10-17' / 'photo.jpg'
    photo.parent.mkdir(parents=True)
    photo.write_bytes(b'photograph-preserved-for-restore')
    (data / 'private-key.txt').write_text('not-in-backup')
    with sqlite3.connect(data / 'rice-cloud.sqlite3') as db:
        db.execute('CREATE TABLE photos(path TEXT, checksum TEXT)')
        db.execute('INSERT INTO photos VALUES(?,?)', (photo.relative_to(data).as_posix(),
                                                   hashlib.sha256(photo.read_bytes()).hexdigest()))
    return data


def test_backup_restores_records_and_referenced_photos_only(tmp_path):
    data = storage(tmp_path)
    archive = backup.create_backup(data, tmp_path / 'backups')
    assert backup.verify_backup(archive) == 1
    with tarfile.open(archive) as packed:
        assert 'private-key.txt' not in packed.getnames()
        assert 'rice-cloud.sqlite3' in packed.getnames()


def test_missing_photo_fails_backup_instead_of_claiming_success(tmp_path):
    data = storage(tmp_path)
    next((data / 'photos').rglob('*.jpg')).unlink()
    with pytest.raises(RuntimeError, match='missing'):
        backup.create_backup(data, tmp_path / 'backups')


def test_backup_rejects_reference_outside_storage(tmp_path):
    data = storage(tmp_path)
    outside = tmp_path / 'outside.jpg'
    outside.write_bytes(b'private')
    with sqlite3.connect(data / 'rice-cloud.sqlite3') as db:
        db.execute('UPDATE photos SET path=?', ('../outside.jpg',))
    with pytest.raises(RuntimeError, match='outside'):
        backup.create_backup(data, tmp_path / 'backups')
