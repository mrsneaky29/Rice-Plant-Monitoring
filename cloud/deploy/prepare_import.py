"""Create a credential-free data migration archive from the original hub, read-only."""
import argparse
from contextlib import closing
from pathlib import Path
import sqlite3
import tarfile

ROOT = Path(__file__).resolve().parents[1]

if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('hub_data', type=Path)
    args = parser.parse_args()
    stage = ROOT / '.private' / 'legacy-import'
    stage.mkdir(parents=True, exist_ok=True)
    snapshot = stage / 'monitoring.sqlite'
    with closing(sqlite3.connect(f'file:{(args.hub_data / "monitoring.sqlite").resolve().as_posix()}?mode=ro', uri=True)) as source:
        with closing(sqlite3.connect(snapshot)) as target:
            source.backup(target)
            # Historical hub settings can contain API credentials. Preserve only non-secret settings.
            with target:
                target.execute("DELETE FROM settings WHERE lower(key) LIKE '%key%' OR lower(key) LIKE '%token%' OR lower(key) LIKE '%password%' OR lower(key) LIKE '%secret%'")
            if target.execute('PRAGMA integrity_check').fetchone()[0] != 'ok':
                raise RuntimeError('Legacy snapshot failed validation')
    photos_root = (args.hub_data / 'photos').resolve()
    packed = stage / 'history.tar.gz'
    count = 0
    with tarfile.open(packed, 'w:gz') as archive:
        archive.add(snapshot, arcname='monitoring.sqlite')
        for photo in sorted(photos_root.rglob('*')):
            if photo.is_file() and not photo.is_symlink() and photo.suffix.lower() in {'.jpg', '.jpeg'}:
                archive.add(photo, arcname=(Path('photos') / photo.relative_to(photos_root)).as_posix())
                count += 1
    print(f'Prepared read-only history snapshot and {count} original JPEG copies for private migration.')
