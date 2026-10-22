"""Activate prepared analysis settings over SSH; never puts secrets in arguments.

Run only after configure_analysis.py has verified account access to the model.
This helper never creates analysis jobs or submits an OpenAI request.
"""
from __future__ import annotations

import ast
import base64
import inspect
import json
import os
from pathlib import Path
import re
import shlex
import sqlite3
import stat
import subprocess
import sys
import tempfile
import time
import urllib.request

MODEL = "gpt-6.1-sol"
NAMES = ("RICE_OPENAI_API_KEY", "RICE_ANALYSIS_ENABLED", "RICE_ANALYSIS_MODEL", "RICE_ANALYSIS_PASSES")
ROOT = Path(__file__).resolve().parents[1]


def assignments(text: str) -> dict[str, str]:
    result = {}
    for line in text.splitlines():
        match = re.match(r"^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=(.*)$", line)
        if not match:
            continue
        name, value = match.groups()
        value = value.strip()
        if value.startswith(('"', "'")):
            parts = shlex.split(value, comments=False, posix=True)
            if len(parts) != 1:
                raise ValueError("Invalid private environment assignment")
            value = parts[0]
        if name in result:
            raise ValueError("Duplicate private environment assignment")
        result[name] = value
    return result


def validate_config(config: dict[str, str]) -> dict[str, str]:
    if set(config) != set(NAMES):
        raise ValueError("Exactly four analysis settings are required")
    if any(not isinstance(v, str) or any(ord(c) < 32 or ord(c) == 127 for c in v) for v in config.values()):
        raise ValueError("Analysis settings contain unsupported control characters")
    if not config["RICE_OPENAI_API_KEY"].strip():
        raise ValueError("The private analysis key is missing")
    if config["RICE_ANALYSIS_MODEL"] != MODEL:
        raise ValueError("The requested analysis model has not been configured")
    if config["RICE_ANALYSIS_ENABLED"] != "true" or config["RICE_ANALYSIS_PASSES"] != "1":
        raise ValueError("Activation requires enabled=true and exactly one analysis pass")
    return config


def load_config(path: Path) -> dict[str, str]:
    # Do not parse or forward unrelated private settings.
    selected = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=", line)
        if match and match.group(1) in NAMES:
            selected.append(line)
    return validate_config(assignments("\n".join(selected)))


def merge_environment(original: str, config: dict[str, str]) -> str:
    validate_config(config)
    retained = []
    for line in original.splitlines(keepends=True):
        match = re.match(r"^[ \t]*([A-Za-z_][A-Za-z0-9_]*)[ \t]*=", line)
        if not match or match.group(1) not in NAMES:
            retained.append(line)
    text = "".join(retained)
    if text and not text.endswith("\n"):
        text += "\n"
    for name in NAMES:
        value = config[name].replace("\\", "\\\\").replace('"', '\\"')
        text += name + '="' + value + '"\n'
    return text


def archive_guard_present(source: str) -> bool:
    expected = ast.parse('ANALYSIS_ENABLED and request.headers.get("x-rice-archive-import", "").lower() != "true"', mode="eval").body
    for node in ast.walk(ast.parse(source)):
        if isinstance(node, (ast.AsyncFunctionDef, ast.FunctionDef)) and node.name == "upload_photo":
            for condition in ast.walk(node):
                if isinstance(condition, ast.If) and ast.dump(condition.test) == ast.dump(expected):
                    for child in condition.body:
                        for item in ast.walk(child):
                            if isinstance(item, ast.Constant) and isinstance(item.value, str) and "INSERT INTO analysis_jobs" in item.value:
                                return True
    return False


def atomic_private_write(path: Path, data: bytes) -> None:
    fd, temporary = tempfile.mkstemp(prefix=".analysis-env-", dir=path.parent)
    try:
        os.fchmod(fd, 0o600)
        os.fchown(fd, 0, 0)
        with os.fdopen(fd, "wb") as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        directory = os.open(path.parent, os.O_RDONLY)
        try:
            os.fsync(directory)
        finally:
            os.close(directory)
    finally:
        if os.path.exists(temporary):
            os.unlink(temporary)


def remote_activate(config: dict[str, str]) -> None:
    import fcntl
    validate_config(config)
    if os.geteuid() != 0:
        raise RuntimeError("Root access is required")
    env_path = Path("/etc/rice-cloud/server.env")
    info = env_path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_uid != 0:
        raise RuntimeError("Private environment must be a root-owned regular file")
    with (env_path.parent / ".analysis-activation.lock").open("a") as lock:
        os.chmod(lock.name, 0o600)
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        original = env_path.read_bytes()
        existing = assignments(original.decode("utf-8"))
        if existing.get("RICE_ANALYSIS_ENABLED", "false").lower() in {"1", "true", "yes"}:
            raise RuntimeError("Analysis is already enabled; refusing repeated activation")
        source = Path("/opt/rice-cloud/current/server/app.py").read_text(encoding="utf-8")
        if not archive_guard_present(source):
            raise RuntimeError("The deployed archive-import analysis exclusion is missing")
        if not Path("/opt/rice-cloud/current/server/analysis_worker.py").is_file():
            raise RuntimeError("The deployed analysis worker is missing")
        pid_result = subprocess.run(["systemctl", "show", "rice-cloud.service", "--property=MainPID", "--value"], capture_output=True, text=True, check=True)
        pid = int(pid_result.stdout.strip())
        if pid <= 0:
            raise RuntimeError("The current service is not running")
        process_env = dict(item.split(b"=", 1) for item in Path(f"/proc/{pid}/environ").read_bytes().split(b"\0") if b"=" in item)
        if process_env.get(b"RICE_ANALYSIS_ENABLED", b"false").lower() in {b"1", b"true", b"yes"}:
            raise RuntimeError("The running service already has analysis enabled")
        data_dir = existing.get("RICE_DATA_DIR", "/opt/rice-cloud/current/server/data")
        db_path = Path(existing.get("RICE_DB_PATH", str(Path(data_dir) / "rice-cloud.sqlite3")))
        with sqlite3.connect(db_path.resolve().as_uri() + "?mode=ro", uri=True) as connection:
            pending = connection.execute("SELECT count(*) FROM analysis_jobs WHERE state NOT IN ('completed','failed')").fetchone()[0]
        if pending:
            raise RuntimeError("Existing pending or uncertain analysis jobs require review before activation")
        backup = env_path.with_name("server.env.before-analysis-" + time.strftime("%Y%m%dT%H%M%SZ", time.gmtime()) + "-" + os.urandom(4).hex())
        atomic_private_write(backup, original)
        atomic_private_write(env_path, merge_environment(original.decode("utf-8"), config).encode("utf-8"))
        try:
            subprocess.run(["systemctl", "restart", "rice-cloud.service"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True, timeout=45)
            ready = False
            for _ in range(15):
                try:
                    with urllib.request.urlopen("http://127.0.0.1:8796/api/health", timeout=2) as response:
                        ready = response.status == 200
                    if ready:
                        break
                except Exception:
                    time.sleep(1)
            if not ready:
                raise RuntimeError("The service did not become healthy")
        except Exception:
            atomic_private_write(env_path, original)
            subprocess.run(["systemctl", "restart", "rice-cloud.service"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=45)
            raise RuntimeError("Activation failed; the previous private environment was restored") from None
        print("Automatic analysis enabled for future ordinary uploads. Historical photos were not queued.")
        print("Private rollback copy retained at " + str(backup))


def remote_program(config: dict[str, str]) -> str:
    validate_config(config)
    imports = "import ast, base64, json, os, re, shlex, sqlite3, stat, subprocess, tempfile, time, urllib.request\nfrom pathlib import Path\n"
    constants = "MODEL = " + repr(MODEL) + "\nNAMES = " + repr(NAMES) + "\n"
    functions = "\n".join(inspect.getsource(fn) for fn in (assignments, validate_config, merge_environment, archive_guard_present, atomic_private_write, remote_activate))
    encoded = base64.b64encode(json.dumps(config).encode("utf-8")).decode("ascii")
    # Payload appears only on SSH stdin. All remote failures are sanitized.
    invocation = "\ntry:\n    remote_activate(json.loads(base64.b64decode(" + repr(encoded) + ")))\nexcept Exception as exc:\n    print('Analysis activation aborted: ' + (str(exc) if isinstance(exc, (ValueError, RuntimeError)) else type(exc).__name__))\n    raise SystemExit(1)\n"
    return imports + constants + functions + invocation


def main() -> int:
    try:
        config = load_config(ROOT / ".private" / "server.env")
        result = subprocess.run(["ssh", "-o", "BatchMode=yes", "-o", "ConnectTimeout=15", "root@206.189.140.255", "python3", "-"], input=remote_program(config), text=True, capture_output=True, timeout=120)
        # Remote stderr can contain interpreter source; never display it.
        for line in result.stdout.splitlines():
            if line.startswith(("Automatic analysis enabled", "Historical photos", "Private rollback copy retained", "Analysis activation aborted:")):
                print(line)
        if result.returncode:
            print("Analysis was not confirmed enabled. SSH or remote activation failed; credential output was suppressed.")
        return result.returncode
    except Exception:
        print("Analysis activation could not run; private configuration or SSH setup needs attention.")
        return 1


if __name__ == "__main__":
    sys.exit(main())
