import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import subprocess
import sys

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "firmware_version.py"
spec = importlib.util.spec_from_file_location("firmware_version", SCRIPT)
versioning = importlib.util.module_from_spec(spec)
spec.loader.exec_module(versioning)


class FirmwareVersionTests(unittest.TestCase):
    def test_custom_version_is_unique_and_fits_app_descriptor(self):
        env = {"GITHUB_REF_TYPE": "branch", "GITHUB_REF_NAME": versioning.CUSTOM_BRANCH,
               "GITHUB_SHA": "b580283" + "0" * 33, "GITHUB_ACTIONS": "true"}
        with patch.dict(os.environ, env, clear=True):
            version, custom = versioning.select_version()
        self.assertEqual(version, "v1.17.0-viron71-c13-gb580283")
        self.assertTrue(custom)
        self.assertLessEqual(len(version.encode()), 31)

    def test_tag_uses_exact_release_version(self):
        with patch.dict(os.environ, {"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v1.17.1"}, clear=True):
            self.assertEqual(versioning.select_version(), ("v1.17.1", False))

    def test_local_custom_branch_marks_modified_source(self):
        with patch.dict(os.environ, {}, clear=True), patch.object(versioning, "git") as git:
            git.side_effect = [versioning.CUSTOM_BRANCH, "a" * 40, " M main/config.h"]
            version, custom = versioning.select_version()
        self.assertEqual(version, "v1.17.0-viron71-c13-gaaaaaaa-d")
        self.assertTrue(custom)

    def test_other_branches_keep_git_description(self):
        with patch.dict(os.environ, {"GITHUB_REF_NAME": "main"}, clear=True), patch.object(versioning, "git", return_value="v1.17.0-3-g123abcd"):
            self.assertEqual(versioning.select_version(), ("v1.17.0-3-g123abcd", False))

    def test_invalid_sha_and_oversize_version_fail_before_build(self):
        for env in ({"GITHUB_REF_NAME": versioning.CUSTOM_BRANCH, "GITHUB_SHA": "bad"},
                    {"GITHUB_REF_TYPE": "tag", "GITHUB_REF_NAME": "v" * 32}):
            with patch.dict(os.environ, env, clear=True), self.assertRaises(ValueError):
                versioning.select_version()

    def test_actions_output_contains_publish_decision(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "output"
            env = dict(os.environ, GITHUB_REF_TYPE="branch", GITHUB_REF_NAME=versioning.CUSTOM_BRANCH,
                       GITHUB_SHA="c" * 40, GITHUB_ACTIONS="true", GITHUB_OUTPUT=str(output))
            subprocess.run([sys.executable, str(SCRIPT)], env=env, check=True, capture_output=True)
            self.assertEqual(output.read_text(), "version=v1.17.0-viron71-c13-gccccccc\ncustom=true\n")


if __name__ == "__main__":
    unittest.main()
