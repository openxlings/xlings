// xdesk.compositor: a Wayland compositor whose output is an opensal surface.
//
// The picture is the scene wlroots renders into its own buffers (headless
// backend, pixman renderer); each frame's damaged rectangles go out as
// opensal.wire frames, and opensal's events come back in as the seat's input.
export module xdesk.compositor;

import std;
import opensal;

export namespace xdesk::compositor {

// One frame of what the compositor shows, as BGRA rows (tests, and the
// probe that the stack works on a machine).
struct Frame {
    opensal::Size size;
    std::uint32_t stride { 0 };
    std::vector<std::uint8_t> pixels;
};
std::expected<Frame, std::string> probe(opensal::Size size);

}  // namespace xdesk::compositor
