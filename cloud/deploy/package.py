"""Package only public source files, with credential paths excluded by construction."""
import argparse
from pathlib import Path
import tarfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
EXCLUDED_PARTS = {'.private', '.venv', '.git', '__pycache__', '.pytest_cache', 'data', 'node_modules'}
EXCLUDED_NAMES = {'secrets.h', 'cloud_secrets.h', '.env', 'PHOTO_INVENTORY.md', 'AGY_REVIEW.md', 'AGY_RESTORE_HANDOFF.md'}
PUBLIC_ROOT_FILES = {'.gitignore', 'README.md', 'API_CONTRACT.md', 'Configure-Private.ps1'}
PUBLIC_DIRECTORIES = {'server', 'deploy', 'firmware', 'review_tests'}


def sources(server_only: bool = False):
    for path in sorted(ROOT.rglob('*')):
        relative = path.relative_to(ROOT)
        if len(relative.parts) == 1 and path.name not in PUBLIC_ROOT_FILES:
            continue
        if len(relative.parts) > 1 and relative.parts[0] not in PUBLIC_DIRECTORIES:
            continue
        # Review runs may leave arbitrary temporary fixtures beside the tests.
        # Publish only the actual top-level Python/C++ regression sources.
        if relative.parts[0] == 'review_tests' and (len(relative.parts) != 2 or path.suffix not in {'.py', '.cpp'}):
            continue
        if path.is_symlink() or not path.is_file() or set(relative.parts) & EXCLUDED_PARTS or path.name in EXCLUDED_NAMES:
            continue
        if any(part.startswith('.pytest') for part in relative.parts):
            continue
        if any(part.startswith('build') or part == 'dist' for part in relative.parts[:-1]):
            continue
        if path.suffix.lower() in {'.bin', '.elf', '.map', '.zip', '.gz', '.bundle', '.sqlite', '.sqlite3', '.db', '.pyc',
                                   '.jpg', '.jpeg', '.png', '.webp', '.pem', '.key', '.p12', '.pfx'} or '.sqlite' in path.name.lower():
            continue
        if server_only and relative.parts[0] not in {'server', 'deploy'}:
            continue
        yield path, relative


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('output', type=Path)
    parser.add_argument('--server-only', action='store_true')
    args = parser.parse_args()
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.output.name.endswith('.tar.gz'):
        with tarfile.open(args.output, 'w:gz') as archive:
            for path, relative in sources(args.server_only):
                archive.add(path, arcname=relative.as_posix())
    else:
        with zipfile.ZipFile(args.output, 'w', zipfile.ZIP_DEFLATED) as archive:
            for path, relative in sources(args.server_only):
                archive.write(path, relative.as_posix())
    print(f'Created credential-free source package: {args.output.name}')
