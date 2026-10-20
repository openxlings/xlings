// xdesk.compositor: a Wayland compositor whose output is an opensal surface.
//
// The picture is the scene wlroots renders into its own buffers (headless
// backend, pixman renderer); each frame's damaged rectangles go out as
// opensal.wire frames, and opensal's events come back in as the seat's input.
export module xdesk.compositor;

import std;
import opensal;
export import :options;

export namespace xdesk::compositor {

// A session until the provider closes its stream: 0 then; 1 when the
// compositor could not start.
int run(const Options& o);

// One frame of what the compositor shows, as BGRA rows (tests, and the
// probe that the stack works on a machine).
struct Frame {
    opensal::Size size;
    std::uint32_t stride { 0 };
    std::vector<std::uint8_t> pixels;
};
std::expected<Frame, std::string> probe(opensal::Size size);

// A complete keymap for `layout`, as xkbcommon writes one (needs the XKB data;
// what the program carries is generated with it once, see keymap.cppm).
std::expected<std::string, std::string> keymap_text(std::string_view layout);
// The US keymap the program carries.
std::string_view builtin_keymap();

// The desktop's keys as a Markdown table, by group (English or Chinese):
// what docs/generated/xdesk-keys.md is (design 2026-10-20 §12.4).
std::string keys_markdown(bool zh);

}  // namespace xdesk::compositor
