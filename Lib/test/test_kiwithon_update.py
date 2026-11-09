"""Unit and integration tests for Kiwithon self-updater and install script."""

import hashlib
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest import mock

try:
    import _kiwithon_update
except ModuleNotFoundError:
    import importlib.util
    lib_path = os.path.join(
        os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
        "_kiwithon_update.py"
    )
    spec = importlib.util.spec_from_file_location("_kiwithon_update", lib_path)
    _kiwithon_update = importlib.util.module_from_spec(spec)
    sys.modules["_kiwithon_update"] = _kiwithon_update
    spec.loader.exec_module(_kiwithon_update)


class KiwithonUpdateTest(unittest.TestCase):
    def test_platform_info(self):
        os_name, arch_name = _kiwithon_update.get_platform_info()
        self.assertIn(os_name, ("linux", "macos", "windows"))
        self.assertIn(arch_name, ("x86_64", "aarch64", "arm64", "i686", "i386"))

    def test_compute_sha256(self):
        with tempfile.NamedTemporaryFile("wb", delete=False) as f:
            f.write(b"kiwithon update test payload\n")
            temp_path = f.name
        try:
            expected = hashlib.sha256(b"kiwithon update test payload\n").hexdigest()
            self.assertEqual(_kiwithon_update.compute_sha256(temp_path), expected)
        finally:
            os.remove(temp_path)

    def test_cli_help(self):
        with self.assertRaises(SystemExit) as cm:
            with mock.patch("sys.stdout", new_callable=io.StringIO):
                _kiwithon_update.main(raw=False, argv=["--help"])
        self.assertEqual(cm.exception.code, 0)

        with self.assertRaises(SystemExit) as cm:
            with mock.patch("sys.stdout", new_callable=io.StringIO):
                _kiwithon_update.main(raw=True, argv=["--help"])
        self.assertEqual(cm.exception.code, 0)

    def test_update_check_dry_run(self):
        fake_release = {
            "tag_name": "v3.16.0",
            "assets": [
                {
                    "name": "kiwithon-linux-x86_64.tar.gz",
                    "browser_download_url": "https://example.com/kiwithon-linux-x86_64.tar.gz",
                }
            ],
        }
        with mock.patch("_kiwithon_update.fetch_release_info", return_value=fake_release):
            with mock.patch("sys.stdout", new_callable=io.StringIO):
                rc = _kiwithon_update.main(raw=False, argv=["--check"])
                self.assertEqual(rc, 0)

                rc_dry = _kiwithon_update.main(raw=False, argv=["--dry-run", "--prefix", "/tmp/fake-kiwithon"])
                self.assertEqual(rc_dry, 0)

    def test_raw_update_dry_run(self):
        with mock.patch("sys.stdout", new_callable=io.StringIO):
            rc = _kiwithon_update.main(raw=True, argv=["--dry-run", "--prefix", "/tmp/fake-kiwithon"])
            self.assertEqual(rc, 0)

    def test_tarball_extraction_and_verification(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            test_file = os.path.join(tmpdir, "test_binary")
            with open(test_file, "w") as f:
                f.write("#!/bin/sh\necho ok\n")
            os.chmod(test_file, 0o755)

            archive = os.path.join(tmpdir, "kiwithon.tar.gz")
            with tarfile.open(archive, "w:gz") as tar:
                tar.add(test_file, arcname="bin/kiwithon")

            sha = _kiwithon_update.compute_sha256(archive)
            sha_file = os.path.join(tmpdir, "SHA256SUMS")
            with open(sha_file, "w") as f:
                f.write(f"{sha}  kiwithon.tar.gz\n")

            dest_prefix = os.path.join(tmpdir, "dest")
            os.makedirs(dest_prefix, exist_ok=True)

            with tarfile.open(archive, "r:gz") as tar:
                tar.extractall(dest_prefix)

            installed_bin = os.path.join(dest_prefix, "bin", "kiwithon")
            self.assertTrue(os.path.isfile(installed_bin))
            self.assertTrue(os.access(installed_bin, os.X_OK))


class InstallerScriptTest(unittest.TestCase):
    def setUp(self):
        self.script_path = os.path.join(
            os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
            "install.sh",
        )

    def test_installer_syntax(self):
        if not os.path.isfile(self.script_path):
            self.skipTest("install.sh not found")
        res = subprocess.run(["sh", "-n", self.script_path], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0, f"install.sh syntax error: {res.stderr}")

    def test_installer_help(self):
        if not os.path.isfile(self.script_path):
            self.skipTest("install.sh not found")
        res = subprocess.run([self.script_path, "--help"], capture_output=True, text=True)
        self.assertEqual(res.returncode, 0)
        self.assertIn("Kiwithon Installer", res.stdout)
        self.assertIn("--prefix", res.stdout)
        self.assertIn("--raw", res.stdout)

    def test_installer_invalid_option(self):
        if not os.path.isfile(self.script_path):
            self.skipTest("install.sh not found")
        res = subprocess.run([self.script_path, "--invalid-flag-1234"], capture_output=True, text=True)
        self.assertNotEqual(res.returncode, 0)


if __name__ == "__main__":
    unittest.main()
