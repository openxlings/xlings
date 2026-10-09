export module xlings.confine.linux_landlock;

import std;
import xlings.confine.implementation;

// Landlock: a write fence the program raises on itself -- the host stays
// visible, so only when asked for by name, never in bwrap's place.
export namespace xlings::confine::backends {
Implementation linux_landlock();
}
