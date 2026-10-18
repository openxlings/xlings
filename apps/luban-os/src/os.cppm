// Luban OS, the app (Luban OS architecture design §14).
//
// A window is a display. It shows one system at a time: the launcher (a list,
// a countdown to the default), then the system itself -- its console here;
// a desktop is opensal's surface -- and when it is off, that it is off. The
// management panel (F2, the gear, or the Host key's menu) is where everything
// else is. More windows are more processes: `luban-os` again, or the menu's
// "new window".
export module luban.os;

export import :core;

export namespace luban::os {

int run(int argc, char* argv[]);

}  // namespace luban::os
