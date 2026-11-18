"""Regression tests for the surround fixture generator's command and output paths."""

import io
import subprocess
import sys
import tempfile
import unittest
from contextlib import chdir, redirect_stderr, redirect_stdout
from pathlib import Path
from unittest.mock import patch

import generate_surround_channel_check as generator


class GenerateSurroundChannelCheckTest(unittest.TestCase):
    """Check that the generator launches FFmpeg only with safe output paths."""

    def test_runs_ffmpeg_with_fixed_names_inside_requested_directory(self):
        """Pass the generated PCM to FFmpeg without passing CLI paths as arguments."""
        with tempfile.TemporaryDirectory() as temp, chdir(temp):
            output = Path.cwd() / "fixtures with spaces"
            with (
                patch.object(sys, "argv", ["generate_surround_channel_check.py", str(output)]),
                patch.object(generator, "channel_check_pcm", return_value=b"pcm"),
                patch.object(subprocess, "run") as run,
                redirect_stdout(io.StringIO()),
            ):
                generator.main()

            self.assertTrue(output.is_dir())
            run.assert_called_once()
            command = run.call_args.args[0]
            self.assertEqual(command[0], "ffmpeg")
            self.assertIn(generator.WAV_NAME, command)
            self.assertIn(generator.SPDIF_NAME, command)
            self.assertNotIn(str(output), command)
            self.assertEqual(run.call_args.kwargs, {"cwd": output.resolve(), "input": b"pcm", "check": True})

    def test_rejects_paths_outside_current_directory(self):
        """Check that a parent traversal and an absolute sibling path both fail."""
        with tempfile.TemporaryDirectory() as temp:
            parent = Path(temp)
            working = parent / "working"
            working.mkdir()
            for output in ("../outside", str(parent / "outside")):
                with self.subTest(output=output), chdir(working):
                    self.assert_cli_rejected(output, "output directory must be within the current directory")
            self.assertFalse((parent / "outside").exists())

    def test_rejects_existing_output(self):
        """An existing surround fixture is preserved and FFmpeg never starts."""
        with tempfile.TemporaryDirectory() as temp, chdir(temp):
            existing = Path(generator.SPDIF_NAME)
            existing.write_bytes(b"keep")
            self.assert_cli_rejected(".", "output already exists")
            self.assertEqual(existing.read_bytes(), b"keep")
            self.assertFalse(Path(generator.WAV_NAME).exists())

    def assert_cli_rejected(self, output, message):
        """Assert a path is rejected before starting any subprocess."""
        with (
            patch.object(sys, "argv", ["generate_surround_channel_check.py", output]),
            patch.object(subprocess, "run") as run,
            redirect_stderr(io.StringIO()) as stderr,
            self.assertRaises(SystemExit) as exit_context,
        ):
            generator.main()

        self.assertEqual(exit_context.exception.code, 2)
        self.assertIn(message, stderr.getvalue())
        run.assert_not_called()
