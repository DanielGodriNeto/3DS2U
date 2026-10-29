"""Synthetic scanner checks; no real credentials or personal identifiers."""
import importlib.util
import pathlib
import unittest

spec = importlib.util.spec_from_file_location("audit", pathlib.Path(__file__).parents[1] / "audit_repository.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)

class AuditTests(unittest.TestCase):
    def test_synthetic_token(self):
        token = b"ghp_" + b"A" * 40
        self.assertIn("service_token", audit.categories(token, ["sample.txt"]))

    def test_profile_paths_and_utf16(self):
        path = "C:" + "\\Users\\" + "Example\\file.txt"
        self.assertIn("personal_absolute_path", audit.categories(path.encode(), ["sample.txt"]))
        self.assertIn("personal_absolute_path", audit.categories(path.encode("utf-16"), ["sample.txt"]))

    def test_private_files(self):
        for name in ("nested/keys.txt", "log.txt", "game.3dsx", "example.pfx"):
            self.assertIn("private_filename", audit.categories(b"", [name]))

    def test_authorship_and_examples(self):
        self.assertEqual([], audit.categories(b"Copyright Example Contributor", ["AUTHORS"]))
        self.assertEqual([], audit.categories(b"SDK_ROOT=/opt/toolchain", [".env.example"]))
        self.assertEqual([], audit.categories(b"\0" * 1024, ["triangle_test.exheader"]))

    def test_key_marker(self):
        marker = b"-----BEGIN " + b"PRIVATE KEY-----"
        self.assertIn("private_key_or_certificate", audit.categories(marker, ["sample.txt"]))

if __name__ == "__main__":
    unittest.main()
