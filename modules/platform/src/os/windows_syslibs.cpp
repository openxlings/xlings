#if defined(_WIN32) && !defined(__CYGWIN__)
#pragma comment(lib, "bcrypt.lib")
// run_elevated (ShellExecuteExW) and is_elevated (OpenProcessToken).
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#endif
