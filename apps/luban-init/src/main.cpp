// luban-init: stage-0 of a machine whose root is a SubOS (SubOS design part 2
// §8.2, part 3 §8), as its own binary. It imports only luban.stage0 -- which
// the layer lint keeps free of the xlings frontend -- so "nothing on stage-0's
// path reads a home's Config" is what the build allows, not what a reviewer
// remembers. A machine boots it as `init=<home>/boot/luban-init`; the older
// `init=<home>/boot/xlings-init` (xlings under that name) still works.
import std;
import luban.stage0;

int main(int argc, char* argv[]) { return luban::stage0::run(argc, argv); }
