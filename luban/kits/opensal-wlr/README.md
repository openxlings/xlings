# opensal-wlr

A wlroots compositor's output as an opensal surface. The bridge runs beside
the compositor: it takes the picture (screencopy), injects input (virtual
keyboard and pointer) and follows the size it is shown at (output
management), all through the compositor's standard Wayland protocols, and
speaks `opensal.wire` on a byte stream. Any compositor that implements those
protocols can be shown this way.

A session without a shell of its own (`X-Luban-Shell=none` in its session
file) asks the window to be it (opensal `Shell`): the bridge tells it the
applications the session lists and its windows (foreign-toplevel), and
starts an application or brings a window forward when it asks.

It speaks the Wayland wire itself, so a static program can carry it without
libwayland.

Depends on: `opensal`, `xdesk-base` (`xdesk.os`).
