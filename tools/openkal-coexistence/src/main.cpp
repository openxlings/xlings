// Does openkal-linux live beside this toolchain's C and C++ runtime in one
// image (xlings SubOS design part 3 §4.3, question 1)? Both write the same
// stdout; the implementation describes itself; the preopens are enumerated.
import std;
import openkal.stream;
import openkal.version;
import openkal.fs;

int main() {
    // kal::write bypasses the C library's stdio buffer: flush before mixing.
    std::println("libstdc++ says: hello");
    std::fflush(nullptr);
    const char line[] = "openkal says: hello\n";
    kal::write(kal::out(), line, sizeof(line) - 1);
    std::println("kal_version={} preopens={}", kal::version(), kal::fs::preopen_count());
    return 0;
}
