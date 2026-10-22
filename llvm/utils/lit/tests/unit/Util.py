# RUN: %{python} %s
# UNSUPPORTED: system-windows

import unittest
import platform
import time

from lit.util import runCommandCached
from lit.LitConfig import LitConfig


class TestCommandCache(unittest.TestCase):
    @staticmethod
    def _lit_config():
        return LitConfig(
            progname="lit",
            path=[],
            diagnostic_level="note",
            useValgrind=False,
            valgrindLeakCheck=False,
            valgrindArgs=[],
            noExecute=False,
            debug=False,
            isWindows=(platform.system() == "Windows"),
            order="smart",
            params={},
        )

    def test_basic(self):
        lit_config = self._lit_config()

        self.assertEqual(lit_config.run_command_cached(["printf", "hi"]), b"hi")
        self.assertNotEqual(lit_config.run_command_cached("ls"), None)

        # Test that arguments (e.g. text=True) get forwarded to subprocess.run
        self.assertEqual(
            lit_config.run_command_cached(["printf", "hi"], text=True), "hi"
        )

        # shell=True is not implied
        self.assertEqual(
            lit_config.run_command_cached("ls -al", allow_failure=True), None
        )
        self.assertNotEqual(
            lit_config.run_command_cached("ls -al", allow_failure=True, shell=True),
            None,
        )

        self.assertEqual(
            lit_config.run_command_cached("exit 0", shell=True, allow_failure=True), b""
        )

    def test_fatal(self):
        lit_config = self._lit_config()

        # Test fatal errors
        fatal_counter = 0

        def wrap_fatal(msg):
            nonlocal fatal_counter
            fatal_counter += 1

        lit_config.fatal = wrap_fatal
        lit_config.run_command_cached(["asdfghjkl"])
        self.assertEqual(fatal_counter, 1)

        self.assertEqual(
            lit_config.run_command_cached(["asdfghjkl"], allow_failure=True), None
        )
        self.assertEqual(
            lit_config.run_command_cached("exit 1", shell=True, allow_failure=True),
            None,
        )
        self.assertEqual(fatal_counter, 1)

    def test_cache(self):
        lit_config = self._lit_config()

        date = lit_config.run_command_cached("date", shell=True)
        self.assertNotEqual(date, None)

        # Ideally, we'd just use `date -Ins` and not need to sleep,
        # but nanosecond support was only added in macOS 15.4-ish.
        # (Note that even if this sleep is too short/too long the
        # test should still pass.)
        time.sleep(2)

        # Second time should be cached, i.e. equal to the first
        self.assertEqual(lit_config.run_command_cached("date", shell=True), date)

    def test_which_output_path_not_cached(self):
        import os
        import tempfile
        import lit.util

        with tempfile.TemporaryDirectory() as td:
            out_dir = os.path.join(td, "Output", "test.tmp")
            bin_dir = os.path.join(td, "bin")
            os.makedirs(out_dir)
            os.makedirs(bin_dir)
            bin_tool = os.path.join(bin_dir, "mytool")
            with open(bin_tool, "w") as f:
                f.write("#!/bin/sh\n")
            paths = out_dir + os.pathsep + bin_dir
            self.assertEqual(
                lit.util.which("mytool", paths),
                os.path.normcase(os.path.abspath(bin_tool)),
            )
            out_tool = os.path.join(out_dir, "mytool")
            with open(out_tool, "w") as f:
                f.write("#!/bin/sh\n")
            self.assertEqual(
                lit.util.which("mytool", paths),
                os.path.normcase(os.path.abspath(out_tool)),
            )


if __name__ == "__main__":
    unittest.main()

