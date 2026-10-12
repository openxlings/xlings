module xdesk.compositor;

import std;
import opensal;
import wlroots;
import freedesktop.wayland.server;

namespace xdesk::compositor {

std::expected<Frame, std::string> probe(opensal::Size size) {
    wl_display* display = wl_display_create();
    if (!display) return std::unexpected("no Wayland display");
    wl_event_loop* loop = wl_display_get_event_loop(display);
    wlr_backend* backend = wlr_headless_backend_create(loop);
    if (!backend) { wl_display_destroy(display); return std::unexpected("no headless backend"); }
    wlr_renderer* renderer = wlr_pixman_renderer_create();
    wlr_allocator* allocator = wlr_allocator_autocreate(backend, renderer);
    if (!renderer || !allocator) { wl_display_destroy(display); return std::unexpected("no renderer"); }
    wlr_scene* scene = wlr_scene_create();
    const float background[4] = {0.11f, 0.13f, 0.17f, 1.0f};
    wlr_scene_rect_create(&scene->tree, static_cast<int>(size.width), static_cast<int>(size.height), background);
    if (!wlr_backend_start(backend)) { wl_display_destroy(display); return std::unexpected("the backend did not start"); }
    wlr_output* output = wlr_headless_add_output(backend, size.width, size.height);
    wlr_output_init_render(output, allocator, renderer);
    wlr_output_state on;
    wlr_output_state_init(&on);
    wlr_output_state_set_enabled(&on, true);
    wlr_output_commit_state(output, &on);
    wlr_output_state_finish(&on);
    wlr_scene_output* so = wlr_scene_output_create(scene, output);

    Frame f;
    wlr_output_state state;
    wlr_output_state_init(&state);
    std::string error;
    if (wlr_scene_output_build_state(so, &state, nullptr) && state.buffer) {
        void* data = nullptr;
        std::uint32_t format = 0;
        std::size_t stride = 0;
        if (wlr_buffer_begin_data_ptr_access(state.buffer, wlr_buffer_data_ptr_access_flag::WLR_BUFFER_DATA_PTR_ACCESS_READ,
                                             &data, &format, &stride)) {
            f.size = size;
            f.stride = static_cast<std::uint32_t>(stride);
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            f.pixels.assign(bytes, bytes + stride * size.height);
            wlr_buffer_end_data_ptr_access(state.buffer);
        } else {
            error = "the frame cannot be read";
        }
    } else {
        error = "nothing was rendered";
    }
    wlr_output_state_finish(&state);
    wlr_scene_node_destroy(&scene->tree.node);
    wlr_allocator_destroy(allocator);
    wlr_renderer_destroy(renderer);
    wl_display_destroy_clients(display);
    wlr_backend_destroy(backend);
    wl_display_destroy(display);
    if (!error.empty()) return std::unexpected(error);
    return f;
}

}  // namespace xdesk::compositor
