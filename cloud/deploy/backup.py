"""Create a self-contained local backup; never reads configuration credentials."""
import argparse
from contextlib import closing
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import sqlite3
import tarfile
import tempfile
import time
import uuid


def relative_name(value: str) -> str:
    """Require portable, canonical paths before reading or extracting files."""
    if not isinstance(value, str):
        raise RuntimeError('Backup path is outside storage or invalid')
    path = PurePosixPath(value)
    if (not value or path.is_absolute() or path.as_posix() != value
            or '\\' in value or ':' in value or '..' in path.parts):
        raise RuntimeError('Backup path is outside storage or invalid')
    return value


def photo_references(database: sqlite3.Connection) -> dict[str, str]:
    references = {}
    for relative, checksum in database.execute('SELECT path,checksum FROM photos'):
        relative_name(relative)
        if not relative.startswith('photos/'):
            raise RuntimeError('Referenced photograph is outside photo storage')
        if not isinstance(checksum, str) or not re.fullmatch(r'[0-9a-f]{64}', checksum):
            raise RuntimeError('Database photograph checksum is invalid')
        if relative in references:
            raise RuntimeError('Database contains duplicate photograph paths')
        references[relative] = checksum
    return references


def create_backup(data: Path, destination: Path) -> Path:
    data = data.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    stamp = time.strftime('rice-cloud-%Y%m%d-%H%M%S', time.gmtime())
    output = destination / f'{stamp}-{uuid.uuid4().hex}.tar.gz'
    with tempfile.TemporaryDirectory(dir=destination) as temporary:
        snapshot = Path(temporary) / 'rice-cloud.sqlite3'
        with closing(sqlite3.connect(f'file:{(data / "rice-cloud.sqlite3").as_posix()}?mode=ro', uri=True)) as source:
            with closing(sqlite3.connect(snapshot)) as target:
                source.backup(target)
                if target.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
                    raise RuntimeError('Snapshot integrity check failed')
        with closing(sqlite3.connect(snapshot)) as database:
            # The upload-time database checksums are the source of truth. A new
            # hash of already-corrupted disk bytes must not bless corruption.
            manifest = photo_references(database)
        partial = Path(temporary) / 'backup.partial'
        with tarfile.open(partial, 'w:gz') as archive:
            archive.add(snapshot, arcname='rice-cloud.sqlite3')
            for relative in manifest:
                photo = (data / relative).resolve()
                if data not in photo.parents or not photo.is_file():
                    raise RuntimeError('A referenced photograph is missing or outside storage')
                archive.add(photo, arcname=relative)
            manifest_path = Path(temporary) / 'manifest.json'
            manifest_path.write_text(json.dumps(manifest, indent=2), encoding='utf-8')
            archive.add(manifest_path, arcname='manifest.json')
        # Verify the bytes actually archived, including changes during copying,
        # before the archive is visible as a completed backup. Failed staging
        # files are removed with this task-owned temporary directory.
        verify_backup(partial)
        partial.replace(output)
    return output


def verify_backup(archive_path: Path) -> int:
    with tempfile.TemporaryDirectory() as temporary:
        root = Path(temporary)
        with tarfile.open(archive_path, 'r:gz') as archive:
            names = set()
            files = set()
            for member in archive.getmembers():
                name = relative_name(member.name.rstrip('/') if member.isdir() else member.name)
                if name in names:
                    raise RuntimeError('Backup contains duplicate archive paths')
                names.add(name)
                if member.isfile():
                    files.add(name)
                elif not member.isdir():
                    raise RuntimeError('Backup contains a nonregular archive member')
            archive.extractall(root, filter='data')
        database_path = root / 'rice-cloud.sqlite3'
        if not database_path.is_file():
            raise RuntimeError('Restored database is missing')
        with closing(sqlite3.connect(f'file:{database_path.as_posix()}?mode=ro', uri=True)) as database:
            if database.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
                raise RuntimeError('Restored database failed integrity check')
            references = photo_references(database)
        try:
            manifest = json.loads((root / 'manifest.json').read_text(encoding='utf-8'))
        except (OSError, ValueError) as exc:
            raise RuntimeError('Restored manifest is missing or invalid') from exc
        if not isinstance(manifest, dict) or manifest != references:
            raise RuntimeError('Restored manifest does not match database photograph checksums and references')
        expected_files = {'rice-cloud.sqlite3', 'manifest.json'} | references.keys()
        if expected_files - files:
            raise RuntimeError('Restored backup is missing referenced files')
        if files - expected_files:
            raise RuntimeError('Restored backup contains unreferenced files')
        for relative, expected in references.items():
            photo = (root / relative).resolve()
            if root not in photo.parents or not photo.is_file():
                raise RuntimeError('Restored photograph is missing or outside storage')
            with photo.open('rb') as image:
                actual = hashlib.file_digest(image, 'sha256').hexdigest()
            if actual != expected:
                raise RuntimeError('Restored photograph checksum mismatch')
        return len(references)


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--data', type=Path, default=Path('/var/lib/rice-cloud'))
    parser.add_argument('--destination', type=Path, default=Path('/var/backups/rice-cloud'))
    parser.add_argument('--verify', type=Path)
    args = parser.parse_args()
    if args.verify:
        print(f'Verified restore: {verify_backup(args.verify)} photographs')
    else:
        result = create_backup(args.data, args.destination)
        print(f'Backup created and restore verified: {result}')
