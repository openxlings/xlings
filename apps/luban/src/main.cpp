// luban: one binary, two names (Luban design §A1.1).
//
//   luban-init   stage-0 of a machine whose root is a SubOS (SubOS design part
//                2 §8.2, part 3 §8). It imports only luban.stage0 -- which the
//                layer lint keeps free of the xlings frontend -- so "nothing on
//                stage-0's path reads a home's Config" is what the build
//                allows. A machine boots it as `init=<home>/boot/luban-init`;
//                the older `init=<home>/boot/xlings-init` still works.
//   luban        the Luban OS management tool (luban.cli).
import std;
import luban.stage0;
import luban.cli;

int main(int argc, char* argv[]) {
    const auto self = argc > 0 && argv[0] ? std::filesystem::path(argv[0]).filename().string() : std::string{};
    if (self.starts_with("luban-init")) return luban::stage0::run(argc, argv);
    return luban::cli::run(argc, argv);
}
