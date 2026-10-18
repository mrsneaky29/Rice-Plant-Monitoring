"""Backup failure/recovery regressions. All data is synthetic temporary storage."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sqlite3
import tarfile

import pytest


SPEC = importlib.util.spec_from_file_location(
    'backup_integrity_regressions', Path(__file__).resolve().parents[1] / 'deploy' / 'backup.py')
backup = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(backup)
RELATIVE = 'photos/plant-2/2026-10-17/photo.jpg'
ORIGINAL = b'synthetic-original-photograph'
CHECKSUM = hashlib.sha256(ORIGINAL).hexdigest()


@pytest.fixture
def data(tmp_path):
    root = tmp_path / 'data'
    photo = root / RELATIVE
    photo.parent.mkdir(parents=True)
    photo.write_bytes(ORIGINAL)
    with sqlite3.connect(root / 'rice-cloud.sqlite3') as db:
        db.execute('CREATE TABLE photos(path TEXT NOT NULL, checksum TEXT NOT NULL)')
        db.execute('INSERT INTO photos VALUES(?,?)', (RELATIVE, CHECKSUM))
    return root


def make_archive(data, tmp_path, manifest, payload=ORIGINAL, include_photo=True, duplicate=False):
    staging = tmp_path / 'archive-input'
    staging.mkdir()
    photo = staging / 'photo.jpg'
    photo.write_bytes(payload)
    metadata = staging / 'manifest.json'
    metadata.write_text(json.dumps(manifest), encoding='utf-8')
    archive_path = tmp_path / 'input.tar.gz'
    with tarfile.open(archive_path, 'w:gz') as archive:
        archive.add(data / 'rice-cloud.sqlite3', arcname='rice-cloud.sqlite3')
        archive.add(metadata, arcname='manifest.json')
        if include_photo:
            archive.add(photo, arcname=RELATIVE)
            if duplicate:
                archive.add(photo, arcname=RELATIVE)
    return archive_path


def test_corrupt_original_cannot_be_published_as_valid_backup(data, tmp_path):
    photo = data / RELATIVE
    photo.write_bytes(b'corrupted-original')
    with pytest.raises(RuntimeError, match='checksum'):
        backup.create_backup(data, tmp_path / 'backups')
    assert not list((tmp_path / 'backups').glob('*.tar.gz'))
    assert photo.read_bytes() == b'corrupted-original'
    assert not list((tmp_path / 'backups').rglob('*.partial'))


def test_restore_rejects_empty_manifest_for_database_photo(data, tmp_path):
    archive = make_archive(data, tmp_path, {}, include_photo=False)
    with pytest.raises(RuntimeError, match='manifest'):
        backup.verify_backup(archive)


def test_restore_rejects_missing_photo_even_with_matching_manifest(data, tmp_path):
    archive = make_archive(data, tmp_path, {RELATIVE: CHECKSUM}, include_photo=False)
    with pytest.raises(RuntimeError, match='missing'):
        backup.verify_backup(archive)


def test_matching_forged_manifest_and_photo_cannot_override_database_checksum(data, tmp_path):
    changed = b'altered-after-upload'
    archive = make_archive(data, tmp_path, {RELATIVE: hashlib.sha256(changed).hexdigest()}, payload=changed)
    with pytest.raises(RuntimeError, match='manifest|checksum'):
        backup.verify_backup(archive)


def test_restore_rejects_unreferenced_manifest_entry(data, tmp_path):
    archive = make_archive(data, tmp_path, {RELATIVE: CHECKSUM, 'photos/extra.jpg': CHECKSUM})
    with pytest.raises(RuntimeError, match='manifest'):
        backup.verify_backup(archive)


def test_restore_rejects_duplicate_archive_paths(data, tmp_path):
    archive = make_archive(data, tmp_path, {RELATIVE: CHECKSUM}, duplicate=True)
    with pytest.raises(RuntimeError, match='duplicate'):
        backup.verify_backup(archive)


def test_previous_archive_format_still_verifies(data, tmp_path):
    archive = make_archive(data, tmp_path, {RELATIVE: CHECKSUM})
    assert backup.verify_backup(archive) == 1


@pytest.mark.parametrize('relative', ['../outside.jpg', '/outside.jpg',
                                    'photos/../private-key.txt', 'photos\\outside.jpg',
                                    'photos/C:outside.jpg', 'photos//outside.jpg'])
def test_noncanonical_database_paths_are_rejected_without_publication(data, tmp_path, relative):
    with sqlite3.connect(data / 'rice-cloud.sqlite3') as db:
        db.execute('UPDATE photos SET path=?', (relative,))
    with pytest.raises(RuntimeError, match='outside|invalid'):
        backup.create_backup(data, tmp_path / 'backups')
    assert not list((tmp_path / 'backups').glob('*.tar.gz'))


@pytest.mark.parametrize('link_type', [tarfile.SYMTYPE, tarfile.LNKTYPE])
def test_restore_rejects_archive_links(data, tmp_path, link_type):
    archive = tmp_path / 'linked.tar.gz'
    with tarfile.open(archive, 'w:gz') as packed:
        link = tarfile.TarInfo(RELATIVE)
        link.type = link_type
        link.linkname = 'rice-cloud.sqlite3'
        packed.addfile(link)
    with pytest.raises(RuntimeError, match='nonregular'):
        backup.verify_backup(archive)


def test_mutation_while_archiving_is_rejected_before_publication(data, tmp_path, monkeypatch):
    real_add = tarfile.TarFile.add

    def changed_add(archive, name, arcname=None, **kwargs):
        if arcname == RELATIVE:
            (data / RELATIVE).write_bytes(b'changed-during-archive-write')
        return real_add(archive, name, arcname=arcname, **kwargs)

    monkeypatch.setattr(tarfile.TarFile, 'add', changed_add)
    with pytest.raises(RuntimeError, match='checksum'):
        backup.create_backup(data, tmp_path / 'backups')
    assert not list((tmp_path / 'backups').glob('*.tar.gz'))


def test_same_second_backups_do_not_overwrite_each_other(data, tmp_path, monkeypatch):
    monkeypatch.setattr(backup.time, 'strftime', lambda *_: 'rice-cloud-20261017-120000')
    first = backup.create_backup(data, tmp_path / 'backups')
    first_bytes = first.read_bytes()
    second = backup.create_backup(data, tmp_path / 'backups')
    assert first != second
    assert first.read_bytes() == first_bytes
    assert backup.verify_backup(first) == backup.verify_backup(second) == 1


def test_empty_database_backup_is_valid(tmp_path):
    data = tmp_path / 'empty-data'
    data.mkdir()
    with sqlite3.connect(data / 'rice-cloud.sqlite3') as db:
        db.execute('CREATE TABLE photos(path TEXT NOT NULL, checksum TEXT NOT NULL)')
    assert backup.verify_backup(backup.create_backup(data, tmp_path / 'backups')) == 0
