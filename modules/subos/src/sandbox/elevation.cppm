export module xlings.subos.elevation;

import std;
import xlings.subos.home_view;

// Running something as the administrator, said and recorded (SubOS design
// part 3 §6.4). Every command the SubOS code runs with administrator rights
// goes through here: it is run as argv (no shell), and a line naming what,
// why and how it ended is appended to <home>/logs/elevation.ndjson -- the
// record a later "who changed this machine" question reads first.
export namespace xlings::subos::elevation {

// What `run` returns when the rights would have to be asked for and nobody
// can answer (sysexits' EX_NOPERM). The command was not run.
inline constexpr int kNobodyToAsk = 77;

// Whether a question for administrator rights reaches a person: not in agent
// mode, standard input is a terminal, and this process is in that terminal's
// foreground. A process in the background of its terminal that asks is
// stopped by the kernel (SIGTTIN) and waits forever -- `self doctor --fix`
// repairing another SubOS through a child once did exactly that.
bool can_ask();

// Run `argv` with administrator rights. When nobody can be asked, rights
// that need no question are still used (already root, or a credential sudo
// holds without asking); otherwise nothing runs and kNobodyToAsk is returned.
int run(const HomeView& home, const std::vector<std::string>& argv, std::string_view purpose);

// One line for a caller to print when `run` returned kNobodyToAsk: what
// needed the rights, and that it has to be run where it can ask.
std::string nobody_to_ask(std::string_view purpose, std::string_view command);

}  // namespace xlings::subos::elevation
