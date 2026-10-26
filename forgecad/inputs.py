"""Fixed SDK downloads and exact source patches; never modify an installed SDK."""
import base64
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import subprocess
import tarfile
import tempfile
import urllib.request


ROOT = Path(__file__).resolve().parent


def sha(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode()


def checked(path, digest):
    if not Path(path).is_file() or sha(path) != digest:
        raise ValueError("SHA256 mismatch: " + str(path))
    return Path(path)


def download(record, cache, seed=None):
    cache = Path(cache)
    cache.mkdir(parents=True, exist_ok=True)
    destination = cache / record["archive_sha256"]
    if destination.exists():
        return checked(destination, record["archive_sha256"])
    descriptor, temporary = tempfile.mkstemp(prefix=".download-", dir=cache)
    try:
        with os.fdopen(descriptor, "wb") as output:
            if seed is not None:
                checked(seed, record["archive_sha256"])
                with Path(seed).open("rb") as source:
                    shutil.copyfileobj(source, output)
            else:
                with urllib.request.urlopen(record["url"], timeout=120) as source:
                    shutil.copyfileobj(source, output)
        checked(temporary, record["archive_sha256"])
        os.replace(temporary, destination)
    finally:
        Path(temporary).unlink(missing_ok=True)  # Only our disposable download.
    return destination


def extract(archive, prefix, destination):
    destination = Path(destination)
    with tarfile.open(archive) as stream:
        members = []
        seen = set()
        for member in stream:
            if member.name == prefix.rstrip("/") and member.isdir():
                continue
            if not member.name.startswith(prefix):
                raise ValueError("archive outside fixed prefix: " + member.name)
            name = member.name[len(prefix):]
            path = PurePosixPath(name)
            if (not name or path.is_absolute() or ".." in path.parts
                    or "\\" in name or ":" in name or name in seen
                    or not (member.isfile() or member.isdir())):
                raise ValueError("unsafe archive member: " + member.name)
            seen.add(name)
            members.append((member, path))
        # Validate the whole archive before writing any member.
        destination.mkdir(parents=True, exist_ok=True)
        for member, path in members:
            output = destination.joinpath(*path.parts)
            for parent in (output, *output.parents):
                if parent.is_symlink():
                    raise ValueError("symlink destination: " + str(parent))
            if member.isdir():
                output.mkdir(parents=True, exist_ok=True)
            else:
                output.parent.mkdir(parents=True, exist_ok=True)
                with stream.extractfile(member) as source, output.open("xb") as target:
                    shutil.copyfileobj(source, target)
                output.chmod(member.mode & 0o777)


def load_lock():
    lock = json.loads((ROOT / "sdk.lock.json").read_text())
    for name, digest in {**lock["resources"], **lock.get("build_resources", {})}.items():
        checked(ROOT / "resources" / name, digest)
    checkout = ROOT.parent
    if (checkout / ".git").exists() and "upstream" in lock:
        commit = subprocess.check_output(["git", "-C", str(checkout), "rev-parse",
                                          lock["upstream"]["tag"] + "^{commit}"], text=True).strip()
        if commit != lock["upstream"]["commit"]:
            raise ValueError("FreeCAD upstream tag identity differs from sdk.lock.json")
        expected = {}
        for name in ["patch-inputs.json", "headless-assembly.json", "qt-literals.json", "headless-dependencies.json"]:
            policy = json.loads((ROOT / "resources" / name).read_text())
            expected.update({path: record["after_sha256"] for path, record in policy["payloads"].items()})
        for name, digest in expected.items():
            checked(checkout / name, digest)
        changed = subprocess.check_output(["git", "-C", str(checkout), "diff", "--name-only",
                                            commit, "--", "src", "cMake", "CMakeLists.txt"], text=True)
        if not set(changed.splitlines()) <= set(expected):
            raise ValueError("SDK checkout has source changes outside the locked patches")
    return lock


def patch(source):
    inputs = json.loads((ROOT / "resources/patch-inputs.json").read_text())
    checked(ROOT / "resources/forgecad-python-v1.patch", inputs["patch_sha256"])
    changes = []
    for name, record in inputs["payloads"].items():
        target = Path(source) / name
        if record["before_sha256"] is None:
            if target.exists():
                raise ValueError("patch target already exists: " + name)
        else:
            checked(target, record["before_sha256"])
        data = base64.b64decode(record["after_base64"], validate=True)
        if hashlib.sha256(data).hexdigest() != record["after_sha256"]:
            raise ValueError("patch payload SHA256 mismatch: " + name)
        changes.append((target, data))
    for target, data in changes:
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)


def patch_headless(source):
    for filename in ["headless-assembly.json", "qt-literals.json", "headless-dependencies.json"]:
        policy = json.loads((ROOT / "resources" / filename).read_text())
        for name, record in policy["payloads"].items():
            path = Path(source) / name
            if sha(path) == record["after_sha256"]:
                continue
            checked(path, record["before_sha256"])
            data = base64.b64decode(record["after_base64"], validate=True)
            if hashlib.sha256(data).hexdigest() != record["after_sha256"]:
                raise ValueError("SDK adaptation SHA256 mismatch")
            path.write_bytes(data)


def prepare(work, seeds=None):
    lock = load_lock()
    sources = Path(work) / "source"
    sources.mkdir(parents=True, exist_ok=True)
    for name, record in lock["archives"].items():
        identity = hashlib.sha256(canonical({"archive": record,
            "patch": lock["resources"] if name == "freecad" else {}})).hexdigest()
        destination = sources / name
        marker = destination / ".forgecad-source.json"
        if marker.exists():
            if json.loads(marker.read_text()) != {"inputs_sha256": identity}:
                raise ValueError("source prepared with different inputs: " + str(destination))
            continue
        if destination.exists():
            raise ValueError("incomplete source retained; choose a new --work directory: " + str(destination))
        archive = download(record, Path(work) / "downloads", (seeds or {}).get(name))
        stage = Path(tempfile.mkdtemp(prefix=".source-", dir=sources))
        try:
            extract(archive, record["root_prefix"], stage)
            if name == "freecad":
                patch(stage)
            (stage / ".forgecad-source.json").write_bytes(canonical({"inputs_sha256": identity}))
            stage.rename(destination)
        finally:
            if stage.exists():
                shutil.rmtree(stage)  # Only our unpublished staging directory.
    freecad = sources / "freecad"
    patch_headless(freecad)
    for path, name in lock["submodules"].items():
        destination = freecad / path
        marker = destination / ".forgecad-source.json"
        if marker.exists():
            continue
        destination.mkdir(parents=True, exist_ok=True)
        shutil.copytree(sources / name, destination, dirs_exist_ok=True)
    return lock
