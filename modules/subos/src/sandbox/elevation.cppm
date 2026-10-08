export module xlings.subos.elevation;

import std;
import xlings.subos.home_view;

// Running something as the administrator, said and recorded (SubOS design
// part 3 §6.4). Every command the SubOS code runs with administrator rights
// goes through here: it is run as argv (no shell), and a line naming what,
// why and how it ended is appended to <home>/logs/elevation.ndjson -- the
// record a later "who changed this machine" question reads first.
export namespace xlings::subos::elevation {

int run(const HomeView& home, const std::vector<std::string>& argv, std::string_view purpose);

}  // namespace xlings::subos::elevation
