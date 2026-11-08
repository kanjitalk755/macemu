# SheepShaver fatal-fault regression

Run with Python 3 and a C++17 compiler (`clang++` by default, overridable with
`CXX`):

```sh
python3 tests/test_fatal_fault_handler.py
```

The test extracts the production `sigsegv_handler` and compiles it with fake
guest CPU state, diagnostic functions, and shutdown callbacks. It checks
recoverable screen/ROM accesses and the ignore-fault preference, then invokes
the fatal path on a worker thread. Both Mach and non-Mach configurations must
retain diagnostics; only the non-Mach configuration may call `QuitEmulator`.

On Apple Silicon macOS, a second test links the production SheepShaver
`sigsegv.cpp` backend and reads a protected page in a disposable subprocess.
The original memory-access signal must terminate that subprocess after the
handler emits diagnostics. A distinct exit code detects an erroneous call to
the shutdown callback. This exercises actual Mach delivery and the unhandled
exception response, without opening emulator windows or using guest assets.
macOS may generate a crash report for the intentionally faulting subprocess.

These checks do not exercise a full guest or all saved debugger exception-port
configurations. The live Mach test is skipped on other hosts. All compilation
artifacts are kept in temporary directories and removed afterward.
