"""Exercise actual local downloads, archive validation and pinned patching."""
import hashlib
import importlib.util
import io
from pathlib import Path
import tarfile
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "inputs.py"


class InputTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.is_file(), "SDK input preparation entry is missing")
        spec = importlib.util.spec_from_file_location("inputs", SCRIPT)
        self.sdk = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.sdk)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def record(self, content=b"pinned archive"):
        source = self.root / "source"
        source.write_bytes(content)
        return {"url": source.as_uri(),
                "archive_sha256": hashlib.sha256(content).hexdigest()}

    def test_download_and_offline_cache_reuse(self):
        record = self.record()
        cached = self.sdk.download(record, self.root / "cache")
        (self.root / "source").unlink()
        self.assertEqual(cached.read_bytes(), b"pinned archive")
        self.assertEqual(self.sdk.download(record, self.root / "cache"), cached)

    def test_wrong_download_hash_is_not_published(self):
        record = self.record()
        record["archive_sha256"] = "0" * 64
        with self.assertRaisesRegex(ValueError, "SHA256"):
            self.sdk.download(record, self.root / "cache")
        self.assertEqual(list((self.root / "cache").iterdir()), [])

    def test_corrupt_cache_is_rejected(self):
        record = self.record()
        cached = self.sdk.download(record, self.root / "cache")
        cached.write_bytes(b"corrupt")
        with self.assertRaisesRegex(ValueError, "SHA256"):
            self.sdk.download(record, self.root / "cache")

    def archive(self, name, link=False):
        archive = self.root / "input.tar"
        with tarfile.open(archive, "w") as stream:
            info = tarfile.TarInfo(name)
            if link:
                info.type = tarfile.SYMTYPE
                info.linkname = "/outside"
                stream.addfile(info)
            else:
                info.size = 2
                stream.addfile(info, io.BytesIO(b"ok"))
        return archive

    def test_extract_regular_source(self):
        self.sdk.extract(self.archive("source/src/a.h"), "source/", self.root / "out")
        self.assertEqual((self.root / "out/src/a.h").read_bytes(), b"ok")

    def test_extract_rejects_traversal_before_writing(self):
        with self.assertRaises(ValueError):
            self.sdk.extract(self.archive("source/../escaped"), "source/", self.root / "out")
        self.assertFalse((self.root / "escaped").exists())

    def test_extract_rejects_links(self):
        with self.assertRaises(ValueError):
            self.sdk.extract(self.archive("source/link", link=True), "source/", self.root / "out")


if __name__ == "__main__":
    unittest.main()
