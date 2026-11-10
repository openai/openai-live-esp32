"""Never apply Mosaico's native-crypto patches to the S3 SDK checkout."""

import os
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import prepare_mosaico


class MosaicoPreparationTest(unittest.TestCase):
    def test_requires_activated_idf(self):
        with patch.dict(os.environ, {}, clear=True), self.assertRaises(ValueError):
            prepare_mosaico.validate_idf()

    def test_rejects_wrong_checkout_before_patching(self):
        with (
            patch.dict(os.environ, {"IDF_PATH": "/fixture/idf"}),
            patch.object(subprocess, "check_output", return_value="wrong\n"),
            self.assertRaises(ValueError),
        ):
            prepare_mosaico.validate_idf()

    def test_accepts_reviewed_checkout(self):
        with (
            patch.dict(os.environ, {"IDF_PATH": "/fixture/idf"}),
            patch.object(
                subprocess,
                "check_output",
                return_value=prepare_mosaico.IDF_COMMIT + "\n",
            ) as git,
        ):
            prepare_mosaico.validate_idf()
            self.assertEqual(
                git.call_args.args[0],
                ["git", "-C", "/fixture/idf", "rev-parse", "HEAD"],
            )

    def test_rejects_missing_git_checkout(self):
        with (
            patch.dict(os.environ, {"IDF_PATH": "/fixture/idf"}),
            patch.object(
                subprocess,
                "check_output",
                side_effect=subprocess.CalledProcessError(128, "git"),
            ),
            self.assertRaises(ValueError),
        ):
            prepare_mosaico.validate_idf()
