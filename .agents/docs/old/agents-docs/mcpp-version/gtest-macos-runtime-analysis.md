# macOS `xlings_tests` Runtime Crash Analysis

## Symptom

`xlings` itself could be built and run normally on macOS, but `xlings_tests` aborted before any test case started.

Observed failure:

```text
malloc: *** error for object ... pointer being freed was not allocated
```

## Reproduction

The failure was reproducible with:

```bash
xmake build xlings_tests
xmake run xlings_tests
```

Build succeeded, but the test binary aborted at process startup.

## Key Evidence

### 1. The problem was not compilation or modules

`xlings_tests` compiled and linked successfully. This ruled out:

- broken compiler toolchain
- failed `gtest` package resolution
- C++23 module compilation failure

### 2. The crash happened before `main()`

`lldb` showed the process aborting during initializer execution:

- `dyld ... runAllInitializersForMain`
- `std::__1::basic_stringstream::~basic_stringstream()`
- `std::__1::ios_base::~ios_base()`
- `std::__1::locale::~locale()`

This meant the process died during static initialization / test registration, before actual test bodies ran.

### 3. `xlings` and `xlings_tests` used different C++ runtime linkage on macOS

Before the fix:

- `xlings` linked against dynamic libc++ from Homebrew LLVM
- `xlings_tests` forced a different setup:
  - `-nostdlib++`
  - `libc++.a`
  - `libc++experimental.a`
  - `-lc++abi`

So the main binary and the test binary did not use the same runtime model.

### 4. The test process still loaded dynamic libc++ at runtime

`DYLD_PRINT_LIBRARIES=1 build/.../xlings_tests` showed that the process still loaded:

- `/usr/lib/libc++.1.dylib`
- `/usr/lib/libc++abi.dylib`

At the same time, `xlings_tests` had also linked static libc++ objects into the executable.

This created the dangerous combination of:

- one libc++ implementation statically linked into the test binary
- another libc++ implementation loaded dynamically by macOS runtime dependencies

## Root Cause

The macOS test target mixed incompatible C++ standard library runtime models.

Objects related to iostream / locale / allocator state were effectively created under one libc++ instance and destroyed under another one. That mismatch caused the early allocator failure:

```text
pointer being freed was not allocated
```

This was not a `gtest` logic bug and not a general toolchain failure. It was a test-target link configuration problem in [`xmake.lua`](/Users/speak/workspace/github/xlings/xmake.lua).

## Fix

The `xlings_tests` macOS target was changed to use the same dynamic libc++ strategy as the main `xlings` binary.

Removed from the test target:

- `-nostdlib++`
- static `libc++.a`
- static `libc++experimental.a`

Replaced with:

- `add_linkdirs(...)`
- `add_rpathdirs(...)`
- `add_syslinks("c++", "c++experimental", "c++abi")`

## Result After Fix

After updating the build description:

```bash
xmake build xlings_tests
xmake run xlings_tests
```

now succeeds on macOS.

Observed result:

- test process starts normally
- `gtest` initializes correctly
- tests run instead of aborting at startup

At verification time:

- `90 tests from 20 test suites`
- `69 passed`
- `21 skipped`

The skipped tests were fixture-availability skips, not runtime crashes.

## Conclusion

The failure came from macOS runtime linkage inconsistency in the test target, not from `gtest` itself.

For this repository, `xlings` and `xlings_tests` should use the same macOS libc++ linkage model unless there is a very strong reason to separate them.
