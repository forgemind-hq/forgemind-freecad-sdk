"""SDK exports must reject incompatible targets, ABI drift and altered files."""
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


class ExportTests(unittest.TestCase):
    def setUp(self):
        script = Path(__file__).resolve().parents[1] / "verify_sdk.py"
        self.assertTrue(script.exists(), "Reusable SDK export verifier is missing")
        spec = importlib.util.spec_from_file_location("verify_sdk", script)
        self.sdk = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.sdk)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        (self.root / "runtime").mkdir()
        (self.root / "sdk/src").mkdir(parents=True)
        (self.root / "runtime/data").write_bytes(b"original")
        self.abi = self.root / "abi.h"
        self.abi.write_bytes(b"private ABI")
        (self.root / "sdk/src/ForgeCADPythonBuildContract.h").write_text("generated")
        self.contract = {"abi_sha256": self.hash(self.abi.read_bytes())}
        (self.root / "contract.json").write_text(json.dumps(self.contract))
        self.value = {"kind": "forgecad.sdk.export.v1", "target": "x86_64-unknown-linux-gnu",
                      "freecad_version": "1.1.3", "python_version": "3.11.9",
                      "contract": "contract.json", "contract_sha256": self.hash(self.sdk.canonical(self.contract)),
                      "sdk": "sdk", "runtime": "runtime", "qt": "qt", "occt_include": "include",
                      "files": {"runtime/data": self.hash(b"original")}}
        self.value["files"].update({name: self.hash((self.root / name).read_bytes()) for name in
                                   ["abi.h", "contract.json", "sdk/src/ForgeCADPythonBuildContract.h"]})
        self.manifest = self.root / "forgecad-sdk-export.json"

    @staticmethod
    def hash(data):
        return hashlib.sha256(data).hexdigest()

    def verify(self):
        self.manifest.write_text(json.dumps(self.value))
        return self.sdk.verify_export(self.manifest, "x86_64-unknown-linux-gnu", self.abi,
                                      require_binaries=False)

    def test_valid_export(self):
        self.assertEqual(self.verify()["runtime"], str(self.root / "runtime"))

    def test_reject_modified_file(self):
        (self.root / "runtime/data").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "SHA256"):
            self.verify()

    def test_reject_target_mismatch(self):
        self.value["target"] = "aarch64-apple-darwin"
        with self.assertRaisesRegex(ValueError, "target"):
            self.verify()

    def test_reject_abi_mismatch(self):
        self.abi.write_bytes(b"different ABI")
        with self.assertRaisesRegex(ValueError, "ABI"):
            self.verify()

    def test_reject_escape(self):
        self.value["files"]["../outside"] = self.hash(b"ignored")
        with self.assertRaisesRegex(ValueError, "path"):
            self.verify()

    def test_reject_incomplete_binary_inventory(self):
        self.manifest.write_text(json.dumps(self.value))
        with self.assertRaisesRegex(ValueError, "binary"):
            self.sdk.verify_export(self.manifest, "x86_64-unknown-linux-gnu", self.abi)


if __name__ == "__main__":
    unittest.main()
