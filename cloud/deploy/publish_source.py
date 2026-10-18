"""Copy only sanitized cloud source into the existing portfolio repository."""
from pathlib import Path
import shutil
import sys

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'deploy'))
from package import sources

if __name__ == '__main__':
    destination = ROOT.parent / 'Rice-Plant-Monitoring' / 'cloud'
    destination.mkdir(exist_ok=True)
    count = 0
    for source, relative in sources():
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        count += 1
    print(f'Copied {count} sanitized cloud source files; original firmware/hub unchanged.')
