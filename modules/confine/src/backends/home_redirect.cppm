export module xlings.confine.home_redirect;

import std;
import xlings.confine.implementation;

// The home redirect of macOS and Windows: HOME / USERPROFILE and the XDG
// directories pointed into the instance -- dotfile isolation, advisory, and
// said to be. The boundary these hosts get is a carrier's (part 3 §5).
export namespace xlings::confine::backends {
Implementation home_redirect();
}
