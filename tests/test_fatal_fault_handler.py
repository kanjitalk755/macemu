#!/usr/bin/env python3
"""Exercise SheepShaver's production fault handler without a VM or ROM.

The handler is compiled verbatim with fake guest state and shutdown callbacks.
The portable checks model fatal delivery on a worker thread. On Apple Silicon,
a separate process also triggers a real fault through the production Mach backend.
"""
import os
from pathlib import Path
import platform
import re
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FatalFaultHandlerTests(unittest.TestCase):
    def extract_handler(self, work):
        source = (ROOT / "SheepShaver/src/kpx_cpu/sheepshaver_glue.cpp").read_text()
        start = re.search(r"^sigsegv_return_t sigsegv_handler\(sigsegv_info_t \*sip\)",
                          source, re.M)
        self.assertIsNotNone(start)
        end = source.index("\n}", start.start()) + 2
        (work / "fault_handler.inc").write_text(source[start.start():end])

    def test_fault_handler(self):
        with tempfile.TemporaryDirectory(prefix="macemu-fault-tests-") as directory:
            work = Path(directory)
            self.extract_handler(work)
            for mach in (0, 1):
                with self.subTest(mach_exceptions=mach):
                    binary = work / f"fault-handler-{mach}"
                    subprocess.run([
                        os.environ.get("CXX", "clang++"), "-std=c++17", "-pthread",
                        "-Wall", "-Wextra", "-Werror", f"-DHAVE_MACH_EXCEPTIONS={mach}",
                        "-I" + str(work), str(ROOT / "tests/fatal_fault_handler.cpp"),
                        "-o", str(binary),
                    ], check=True)
                    subprocess.run([str(binary)], check=True, timeout=10)

    @unittest.skipUnless(platform.system() == "Darwin" and platform.machine() == "arm64",
                         "live Mach test requires an Apple Silicon Mac")
    def test_real_mach_exception(self):
        with tempfile.TemporaryDirectory(prefix="macemu-mach-test-") as directory:
            work = Path(directory)
            self.extract_handler(work)
            binary = work / "fatal-mach"
            subprocess.run([
                os.environ.get("CXX", "clang++"), "-std=c++17", "-pthread",
                "-DHAVE_MACH_EXCEPTIONS=1", "-DHAVE_SIGSEGV_SKIP_INSTRUCTION=1",
                "-DEMULATED_PPC=1", "-DTEST_REAL_MACH_EXCEPTION=1",
                "-I" + str(work), "-I" + str(ROOT / "BasiliskII/src/CrossPlatform"),
                str(ROOT / "tests/fatal_fault_handler.cpp"),
                str(ROOT / "SheepShaver/src/CrossPlatform/sigsegv.cpp"),
                "-o", str(binary),
            ], check=True)
            result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=10)
            self.assertIn(result.returncode, (-signal.SIGBUS, -signal.SIGSEGV), result.stderr)
            self.assertIn("SIGSEGV\n", result.stderr)
            self.assertNotIn("Unexpected QuitEmulator", result.stderr)
            print(f"Live Mach fault terminated with {signal.Signals(-result.returncode).name} "
                  "without calling QuitEmulator.", flush=True)


if __name__ == "__main__":
    unittest.main(verbosity=2)
