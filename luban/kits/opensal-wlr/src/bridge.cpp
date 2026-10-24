module opensal.wlr;

import std;
import opensal;
import opensal.wire;
import xdesk.os;
import opensal.wlr.wayland;

namespace opensal::wlr {

namespace os = xdesk::os;
namespace wl = opensal::wlr::wayland;
using namespace opensal::keys;

namespace {

// The opcodes used here, by interface (each protocol's XML, in order).
namespace op {
// wl_display
constexpr std::uint16_t display_sync = 0, display_get_registry = 1;
constexpr std::uint16_t display_error = 0;
// wl_registry
constexpr std::uint16_t registry_bind = 0;
constexpr std::uint16_t registry_global = 0, registry_global_remove = 1;
// wl_callback
constexpr std::uint16_t callback_done = 0;
// wl_shm, wl_shm_pool, wl_buffer
constexpr std::uint16_t shm_create_pool = 0;
constexpr std::uint16_t pool_create_buffer = 0, pool_destroy = 1;
constexpr std::uint16_t buffer_destroy = 0;
// wl_seat, wl_output
constexpr std::uint16_t seat_get_pointer = 0;
constexpr std::uint16_t output_mode = 1;
// zwlr_screencopy_manager_v1, zwlr_screencopy_frame_v1
constexpr std::uint16_t screencopy_capture_output = 0;
constexpr std::uint16_t frame_copy = 0, frame_destroy = 1, frame_copy_with_damage = 2;
constexpr std::uint16_t frame_buffer = 0, frame_flags = 1, frame_ready = 2, frame_failed = 3, frame_damage = 4,
                        frame_buffer_done = 6;
// zwp_virtual_keyboard_manager_v1, zwp_virtual_keyboard_v1
constexpr std::uint16_t vkbd_manager_create = 0;
constexpr std::uint16_t vkbd_keymap = 0, vkbd_key = 1, vkbd_modifiers = 2;
// zwlr_virtual_pointer_manager_v1, zwlr_virtual_pointer_v1
constexpr std::uint16_t vptr_manager_create = 0, vptr_manager_create_with_output = 2;
constexpr std::uint16_t vptr_motion_absolute = 1, vptr_button = 2, vptr_axis = 3, vptr_frame = 4;
// zwlr_output_manager_v1, its head and mode, a configuration and its head
constexpr std::uint16_t outman_create_configuration = 0;
constexpr std::uint16_t outman_head = 0, outman_done = 1;
constexpr std::uint16_t head_mode = 3, head_finished = 9;
constexpr std::uint16_t config_enable_head = 0, config_apply = 2, config_destroy = 4;
constexpr std::uint16_t config_succeeded = 0, config_failed = 1, config_cancelled = 2;
constexpr std::uint16_t config_head_set_custom_mode = 1;
// zwlr_foreign_toplevel_manager_v1 and a toplevel's handle
constexpr std::uint16_t toplevels_toplevel = 0;
constexpr std::uint16_t toplevel_title = 0, toplevel_app_id = 1, toplevel_state = 4, toplevel_done = 5,
                        toplevel_closed = 6;
constexpr std::uint16_t toplevel_unset_minimized = 3, toplevel_activate = 4, toplevel_destroy = 7;
// ext_output_image_capture_source_manager_v1, ext_image_copy_capture_manager_v1,
// its cursor session, a capture session and its frames
constexpr std::uint16_t sources_create_source = 0;
constexpr std::uint16_t copy_create_pointer_cursor_session = 1;
constexpr std::uint16_t cursor_get_capture_session = 1;
constexpr std::uint16_t cursor_enter = 0, cursor_leave = 1, cursor_hotspot = 3;
constexpr std::uint16_t session_create_frame = 0;
constexpr std::uint16_t session_buffer_size = 0, session_shm_format = 1, session_done = 4, session_stopped = 5;
constexpr std::uint16_t capture_destroy = 0, capture_attach_buffer = 1, capture_damage_buffer = 2, capture_capture = 3;
constexpr std::uint16_t capture_ready = 3, capture_failed = 4;
}  // namespace op

// wlr-data-control (the clipboard, design 2026-10-20 §20 G27).
namespace dc {
constexpr std::uint16_t manager_create_source = 0, manager_get_device = 1;
constexpr std::uint16_t device_set_selection = 0, device_data_offer = 0, device_selection = 1, device_finished = 2;
constexpr std::uint16_t source_offer = 0, source_destroy = 1, source_send = 0, source_cancelled = 1;
constexpr std::uint16_t offer_receive = 0, offer_destroy = 1, offer_offer = 0;
// What the bridge's own selection also offers: one coming back is not read.
constexpr std::string_view kOwn = "application/x-opensal-wlr";
}  // namespace dc

// A toplevel's states (zwlr_foreign_toplevel_handle_v1.state).
constexpr std::uint32_t kStateMinimized = 1, kStateActivated = 2;

constexpr std::uint32_t kShmArgb = 0, kShmXrgb = 1;
// ext_image_copy_capture_frame_v1.failed: unknown, buffer_constraints, stopped.
constexpr std::uint32_t kFailureUnknown = 0;

struct Global { std::uint32_t name { 0 }; std::uint32_t version { 0 }; };

// Another window of this desktop (design 2026-10-20 §11): it mirrors the
// first -- the same frames, its input the desktop's (its size is its own
// business). A bridged desktop ends with its first window.
struct Mirror {
    int fd { -1 };
    opensal::wire::Decoder decoder;
    opensal::Interfaces provided { 0 };
    bool hello { false };
    bool whole { true };            // the next frame it gets is all of it
    opensal::Size told;
    std::uint32_t number { 2 };
    bool gone { false };
    bool primary { false };         // it asked to be the primary (opensal 0.7): the output is its size
    opensal::Size own;              // its size, from its hello and its resizes
};

class Bridge {
public:
    Bridge(const Options& o, wl::Connection c) : o_(o), c_(std::move(c)), start_(std::chrono::steady_clock::now()) {}
    int run();

private:
    void log_(std::string_view s) { if (o_.log) o_.log(s); }
    std::uint32_t now_ms_() const {
        return static_cast<std::uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start_).count());
    }
    std::uint32_t bind_(std::string_view iface, std::uint32_t max_version, std::uint32_t& version);
    bool roundtrip_();
    void handle_(const wl::Event& e);
    bool provider_(std::string_view bytes);
    void input_(const opensal::Event& e);
    void key_(std::uint32_t code, bool down);
    void modifiers_();
    void capture_();
    void copy_();
    void frame_ready_();
    void cursor_start_();
    void cursor_stop_();
    void capture_cursor_();
    void cursor_ready_();
    void resize_(opensal::Size want);
    void shell_(const opensal::Event& e);
    void tell_windows_();
    void tell_applications_();
    bool send_(std::string_view bytes) { return os::write_all(o_.out, bytes); }
    // To the first window and the mirrors (what is not a frame); whether the first took it.
    bool tell_(std::string_view bytes) {
        for (auto& m : mirrors_)
            if (m.hello && !m.gone && !os::write_all(m.fd, bytes)) m.gone = true;
        return send_(bytes);
    }
    void accept_mirror_();
    bool mirror_(Mirror& m, std::string_view bytes);
    void tell_roles_();
    // Rows to a window: into the region both sides map when it takes them
    // there (opensal 0.7 Region), else on its stream; whether it took them.
    bool send_rects_(int fd, opensal::Interfaces provided, const opensal::Pixels& surface, std::span<const opensal::Rect> rects);
    Mirror* primary_mirror_() {
        Mirror* p = nullptr;
        for (auto& m : mirrors_)
            if (m.primary && !m.gone) p = &m;
        return p;
    }

    const Options& o_;
    wl::Connection c_;
    std::chrono::steady_clock::time_point start_;
    std::uint32_t registry_ { 0 };
    std::map<std::string, Global> globals_;
    std::uint32_t sync_ { 0 };
    bool synced_ { false };

    std::uint32_t shm_ { 0 }, seat_ { 0 }, output_ { 0 }, screencopy_ { 0 }, vkbd_manager_ { 0 }, vptr_manager_ { 0 },
                  outman_ { 0 };
    std::uint32_t screencopy_version_ { 1 };
    std::uint32_t vkbd_ { 0 }, vptr_ { 0 };
    opensal::Size output_size_;

    // The capture in flight.
    std::uint32_t frame_ { 0 };
    bool capturing_ { false };
    bool first_ { true };
    struct Offer { std::uint32_t format { 0 }; opensal::Size size; std::uint32_t stride { 0 }; };
    std::optional<Offer> offer_;
    std::vector<opensal::Rect> damage_;
    std::chrono::steady_clock::time_point last_frame_ {};
    std::chrono::steady_clock::time_point retry_at_ {};
    // The buffer it copies into, kept while the size holds.
    os::SharedMemory shm_mem_;
    std::uint32_t pool_ { 0 }, buffer_ { 0 };
    Offer buffer_shape_;

    // Output management: the head (first), the serial to configure against.
    std::uint32_t head_ { 0 };
    std::set<std::uint32_t> heads_, modes_;
    std::uint32_t serial_ { 0 };
    bool have_serial_ { false };
    std::uint32_t config_ { 0 };
    std::optional<opensal::Size> wanted_;
    opensal::Size told_;

    // The provider's stream, and the keyboard's state.
    opensal::wire::Decoder decoder_;
    std::uint32_t mods_ { 0 };
    std::set<std::uint32_t> held_;
    bool provider_gone_ { false };

    // Shell: the compositor's windows (by handle), told when they change, and
    // the programs started for the provider (reaped as they end).
    std::uint32_t toplevels_ { 0 };
    opensal::Interfaces provided_ { 0 };
    std::map<std::uint32_t, opensal::Window> windows_;
    bool windows_changed_ { false };
    std::vector<Application> apps_;
    bool apps_told_ { false };
    std::chrono::steady_clock::time_point apps_read_ {};
    std::vector<int> launched_;

    // The session's pointer apart from its frames (design 2026-10-19 §15.2):
    // when the compositor captures it on its own (ext-image-copy-capture's
    // cursor session) and the provider draws it (CursorImage), each image the
    // compositor's pointer takes goes as 'C' and the frames are captured
    // without it; otherwise it is drawn into them, as before.
    std::uint32_t sources_ { 0 }, copy_capture_ { 0 };
    std::uint32_t cursor_session_ { 0 }, cursor_capture_ { 0 }, cursor_frame_ { 0 };
    bool host_cursor_ { false };
    opensal::CursorImage cursor_;
    bool cursor_argb_ { false };
    os::SharedMemory cursor_mem_;
    std::uint32_t cursor_pool_ { 0 }, cursor_buffer_ { 0 };
    opensal::Size cursor_buffer_size_;
    std::string cursor_sent_;

    // The clipboard: the desktop's selection to the window, the window's to
    // the desktop (wlr-data-control); a selection being read.
    std::uint32_t dc_manager_ { 0 }, dc_device_ { 0 }, dc_source_ { 0 };
    std::map<std::uint32_t, std::vector<std::string>> offers_;
    std::string selection_set_, selection_told_;
    int reading_ { -1 };
    std::string read_;
    void selection_(std::uint32_t offer);
    void set_selection_(const std::string& text);

    // The other windows (displays), at o_.displays.
    int displays_ { -1 };
    std::list<Mirror> mirrors_;
    opensal::Size provider_own_;    // the first window's size: the output's again when a primary that came goes

    // Frames by reference (opensal 0.7 Region): the region the windows map
    // too ($OPENSAL_REGION), and where the next rows go in it.
    std::optional<os::SharedMemory> region_;
    std::size_t region_at_ { 0 };
};

std::uint32_t Bridge::bind_(std::string_view iface, std::uint32_t max_version, std::uint32_t& version) {
    auto it = globals_.find(std::string(iface));
    if (it == globals_.end()) return 0;
    version = std::min(max_version, it->second.version);
    const auto id = c_.new_id();
    c_.send(registry_, op::registry_bind, wl::Args{}.u(it->second.name).str(iface).u(version).u(id));
    return id;
}

bool Bridge::roundtrip_() {
    sync_ = c_.new_id();
    synced_ = false;
    c_.send(wl::kDisplay, op::display_sync, wl::Args{}.u(sync_));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!synced_) {
        if (std::chrono::steady_clock::now() > deadline) return false;
        auto events = c_.poll(200);
        if (!events) return false;
        for (const auto& e : *events) handle_(e);
    }
    return true;
}

void Bridge::handle_(const wl::Event& e) {
    wl::Reader r(e.body);
    if (e.object == wl::kDisplay && e.opcode == op::display_error) {
        const auto object = r.u();
        const auto code = r.u();
        log_(std::format("the compositor refused a request (object {}, code {}): {}", object, code, r.str()));
        return;
    }
    // The callback's id is the server's to delete and ours to use again: it
    // is the sync only until it is done.
    if (sync_ && e.object == sync_ && e.opcode == op::callback_done) { synced_ = true; sync_ = 0; return; }
    if (dc_device_ && e.object == dc_device_) {
        if (e.opcode == dc::device_data_offer) offers_[r.u()] = {};
        else if (e.opcode == dc::device_selection) selection_(r.u());
        else if (e.opcode == dc::device_finished) dc_device_ = 0;
        return;
    }
    if (auto it = offers_.find(e.object); it != offers_.end()) {
        if (e.opcode == dc::offer_offer) it->second.push_back(r.str());
        return;
    }
    if (dc_source_ && e.object == dc_source_) {
        if (e.opcode == dc::source_send) {
            (void)r.str();
            if (const int fd = c_.take_fd(); fd >= 0) {
                (void)os::write_all(fd, selection_set_);
                os::close(fd);
            }
        } else if (e.opcode == dc::source_cancelled) {
            c_.send(dc_source_, dc::source_destroy, wl::Args{});
            dc_source_ = 0;
        }
        return;
    }
    if (e.object == registry_) {
        if (e.opcode == op::registry_global) {
            const auto name = r.u();
            auto iface = r.str();
            const auto version = r.u();
            // The first of each: one output, one seat.
            if (r.ok() && !globals_.contains(iface)) globals_[iface] = {name, version};
        }
        return;
    }
    if (e.object == output_ && e.opcode == op::output_mode) {
        const auto flags = r.u();
        const auto w = r.i(), h = r.i();
        if (r.ok() && (flags & 1) && w > 0 && h > 0) output_size_ = {std::uint32_t(w), std::uint32_t(h)};
        return;
    }
    if (frame_ && e.object == frame_) {
        switch (e.opcode) {
        case op::frame_buffer: {
            Offer f;
            f.format = r.u();
            f.size.width = r.u();
            f.size.height = r.u();
            f.stride = r.u();
            if (r.ok() && (f.format == kShmXrgb || f.format == kShmArgb) && !offer_) offer_ = f;
            if (screencopy_version_ < 3 && offer_) copy_();
            break;
        }
        case op::frame_buffer_done: if (offer_) copy_(); break;
        case op::frame_damage: {
            opensal::Rect d;
            d.x = static_cast<std::int32_t>(r.u());
            d.y = static_cast<std::int32_t>(r.u());
            d.width = r.u();
            d.height = r.u();
            if (r.ok()) damage_.push_back(d);
            break;
        }
        case op::frame_ready: frame_ready_(); break;
        case op::frame_failed:
            c_.send(frame_, op::frame_destroy, wl::Args{});
            frame_ = 0;
            capturing_ = false;
            offer_.reset();
            retry_at_ = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
            break;
        default: break;
        }
        return;
    }
    if (cursor_session_ && e.object == cursor_session_) {
        if (e.opcode == op::cursor_hotspot) {
            const auto x = r.i(), y = r.i();
            cursor_.hotspot_x = static_cast<std::uint32_t>(std::max(0, x));
            cursor_.hotspot_y = static_cast<std::uint32_t>(std::max(0, y));
            if (!cursor_.bytes.empty()) cursor_ready_();
        } else if (e.opcode == op::cursor_leave) {
            if (tell_(opensal::wire::encode('c', "none"))) cursor_sent_ = "none";
        } else if (e.opcode == op::cursor_enter) {
            // Back over the output: its image again, unchanged as it may be
            // (an image is captured only when it changes; design 2026-10-20
            // G29: a nested KWin's pointer stayed hidden after it left once).
            if (!cursor_.bytes.empty()) cursor_ready_();
        }
        return;
    }
    if (cursor_capture_ && e.object == cursor_capture_) {
        switch (e.opcode) {
        case op::session_buffer_size: cursor_.size.width = r.u(); cursor_.size.height = r.u(); break;
        case op::session_shm_format: if (r.u() == kShmArgb) cursor_argb_ = true; break;
        case op::session_done: if (!cursor_frame_) capture_cursor_(); break;
        case op::session_stopped: cursor_stop_(); break;
        default: break;
        }
        return;
    }
    if (cursor_frame_ && e.object == cursor_frame_) {
        if (e.opcode == op::capture_ready) {
            cursor_.bytes.assign(reinterpret_cast<const char*>(cursor_mem_.data), std::size_t{cursor_.size.width} * cursor_.size.height * 4);
            c_.send(cursor_frame_, op::capture_destroy, wl::Args{});
            cursor_frame_ = 0;
            cursor_ready_();
            capture_cursor_();
        } else if (e.opcode == op::capture_failed) {
            // Its size changed (buffer constraints: a new `done` follows), it
            // stopped (`stopped` follows), or it failed otherwise -- then the
            // next image is asked for at once: a pointer whose capture is not
            // asked again keeps the image it had (G29: a stuck text cursor).
            const auto reason = r.u();
            c_.send(cursor_frame_, op::capture_destroy, wl::Args{});
            cursor_frame_ = 0;
            if (reason == kFailureUnknown) capture_cursor_();
        }
        return;
    }
    if (toplevels_ && e.object == toplevels_) {
        if (e.opcode == op::toplevels_toplevel) {
            const auto handle = r.u();
            if (r.ok()) windows_[handle] = opensal::Window{.id = handle};
        }
        return;
    }
    if (auto w = windows_.find(e.object); w != windows_.end()) {
        switch (e.opcode) {
        case op::toplevel_title: w->second.title = r.str(); break;
        case op::toplevel_app_id: w->second.app_id = r.str(); break;
        case op::toplevel_state: {
            const auto states = r.words();
            w->second.focused = std::ranges::contains(states, kStateActivated);
            w->second.minimized = std::ranges::contains(states, kStateMinimized);
            break;
        }
        case op::toplevel_done: windows_changed_ = true; break;
        case op::toplevel_closed:
            c_.send(e.object, op::toplevel_destroy, wl::Args{});
            windows_.erase(w);
            windows_changed_ = true;
            break;
        default: break;
        }
        return;
    }
    if (e.object == outman_) {
        if (e.opcode == op::outman_head) {
            const auto id = r.u();
            heads_.insert(id);
            if (!head_) head_ = id;
        } else if (e.opcode == op::outman_done) {
            serial_ = r.u();
            have_serial_ = r.ok();
            if (wanted_) resize_(*wanted_);
        }
        return;
    }
    if (heads_.contains(e.object)) {
        if (e.opcode == op::head_mode) modes_.insert(r.u());
        else if (e.opcode == op::head_finished) { heads_.erase(e.object); if (head_ == e.object) head_ = 0; }
        return;
    }
    if (config_ && e.object == config_) {
        if (e.opcode == op::config_succeeded || e.opcode == op::config_failed || e.opcode == op::config_cancelled) {
            if (e.opcode != op::config_succeeded) log_("the compositor did not take the window's size");
            c_.send(config_, op::config_destroy, wl::Args{});
            config_ = 0;
        }
        return;
    }
}

// The provider asks for its size; the compositor's output takes it as a
// custom mode (output management), against the serial of its last state.
void Bridge::resize_(opensal::Size want) {
    wanted_ = want;
    if (!outman_ || !head_ || !have_serial_ || config_ || want.width == 0 || want.height == 0) return;
    if (want == output_size_) { wanted_.reset(); return; }
    config_ = c_.new_id();
    c_.send(outman_, op::outman_create_configuration, wl::Args{}.u(config_).u(serial_));
    const auto ch = c_.new_id();
    c_.send(config_, op::config_enable_head, wl::Args{}.u(ch).u(head_));
    c_.send(ch, op::config_head_set_custom_mode, wl::Args{}.i(std::int32_t(want.width)).i(std::int32_t(want.height)).i(0));
    c_.send(config_, op::config_apply, wl::Args{});
    wanted_.reset();
}

// A capture: the compositor answers with the buffer it wants (copy_).
void Bridge::capture_() {
    frame_ = c_.new_id();
    capturing_ = true;
    damage_.clear();
    offer_.reset();
    // The pointer in the frame unless the provider draws it.
    c_.send(screencopy_, op::screencopy_capture_output, wl::Args{}.u(frame_).i(host_cursor_ ? 0 : 1).u(output_));
}

void Bridge::cursor_start_() {
    host_cursor_ = true;
    first_ = true;   // the next frame whole: the pointer drawn in the last goes
    const auto pointer = c_.new_id();
    c_.send(seat_, op::seat_get_pointer, wl::Args{}.u(pointer));
    const auto source = c_.new_id();
    c_.send(sources_, op::sources_create_source, wl::Args{}.u(source).u(output_));
    cursor_session_ = c_.new_id();
    c_.send(copy_capture_, op::copy_create_pointer_cursor_session, wl::Args{}.u(cursor_session_).u(source).u(pointer));
    cursor_capture_ = c_.new_id();
    c_.send(cursor_session_, op::cursor_get_capture_session, wl::Args{}.u(cursor_capture_));
}

// The compositor stopped showing it apart: into the frames again, and the
// provider draws none.
void Bridge::cursor_stop_() {
    host_cursor_ = false;
    first_ = true;
    cursor_capture_ = 0;
    cursor_frame_ = 0;
    if (tell_(opensal::wire::encode('c', "none"))) cursor_sent_ = "none";
}

void Bridge::capture_cursor_() {
    const auto w = cursor_.size.width, h = cursor_.size.height;
    if (!cursor_capture_ || !cursor_argb_ || w == 0 || h == 0 || w > opensal::kMaxCursorSide || h > opensal::kMaxCursorSide) return;
    const std::size_t bytes = std::size_t{w} * h * 4;
    if (!cursor_buffer_ || cursor_buffer_size_ != cursor_.size) {
        if (cursor_buffer_) c_.send(cursor_buffer_, op::buffer_destroy, wl::Args{});
        if (cursor_pool_) c_.send(cursor_pool_, op::pool_destroy, wl::Args{});
        os::release(cursor_mem_);
        auto m = os::shared_memory(bytes);
        if (!m) { log_("no shared memory for the pointer"); return; }
        cursor_mem_ = *m;
        cursor_pool_ = c_.new_id();
        c_.send(shm_, op::shm_create_pool, wl::Args{}.u(cursor_pool_).fd(cursor_mem_.fd).i(std::int32_t(bytes)));
        cursor_buffer_ = c_.new_id();
        c_.send(cursor_pool_, op::pool_create_buffer, wl::Args{}.u(cursor_buffer_).i(0).i(std::int32_t(w)).i(std::int32_t(h))
                                                         .i(std::int32_t(w * 4)).u(kShmArgb));
        cursor_buffer_size_ = cursor_.size;
    }
    // Answered when the pointer's image changes.
    cursor_frame_ = c_.new_id();
    c_.send(cursor_capture_, op::session_create_frame, wl::Args{}.u(cursor_frame_));
    c_.send(cursor_frame_, op::capture_attach_buffer, wl::Args{}.u(cursor_buffer_));
    c_.send(cursor_frame_, op::capture_damage_buffer, wl::Args{}.i(0).i(0).i(std::int32_t(w)).i(std::int32_t(h)));
    c_.send(cursor_frame_, op::capture_capture, wl::Args{});
}

// The image (ARGB is opensal's premultiplied BGRA as bytes), said when it changed.
void Bridge::cursor_ready_() {
    if (cursor_.bytes.size() != std::size_t{cursor_.size.width} * cursor_.size.height * 4) return;
    auto image = cursor_;
    image.hotspot_x = std::min(image.hotspot_x, image.size.width - 1);
    image.hotspot_y = std::min(image.hotspot_y, image.size.height - 1);
    auto message = opensal::wire::encode_cursor_image(image);
    if (message == cursor_sent_) return;
    if (!tell_(message)) { provider_gone_ = true; return; }
    cursor_sent_ = std::move(message);
}

void Bridge::copy_() {
    const auto f = *offer_;
    const std::size_t bytes = std::size_t(f.stride) * f.size.height;
    if (!buffer_ || buffer_shape_.size != f.size || buffer_shape_.stride != f.stride || buffer_shape_.format != f.format) {
        if (buffer_) c_.send(buffer_, op::buffer_destroy, wl::Args{});
        if (pool_) c_.send(pool_, op::pool_destroy, wl::Args{});
        os::release(shm_mem_);
        auto m = os::shared_memory(bytes);
        if (!m) { log_("no shared memory for a frame"); provider_gone_ = true; return; }
        shm_mem_ = *m;
        pool_ = c_.new_id();
        c_.send(shm_, op::shm_create_pool, wl::Args{}.u(pool_).fd(shm_mem_.fd).i(std::int32_t(bytes)));
        buffer_ = c_.new_id();
        c_.send(pool_, op::pool_create_buffer, wl::Args{}.u(buffer_).i(0).i(std::int32_t(f.size.width))
                                                     .i(std::int32_t(f.size.height)).i(std::int32_t(f.stride)).u(f.format));
        buffer_shape_ = f;
        first_ = true;
    }
    // The first copy is whole; after it, only what changed (and the compositor waits until something has).
    const bool damage = screencopy_version_ >= 2 && !first_;
    c_.send(frame_, damage ? op::frame_copy_with_damage : op::frame_copy, wl::Args{}.u(buffer_));
}

void Bridge::frame_ready_() {
    const auto f = buffer_shape_;
    const opensal::Rect whole{0, 0, f.size.width, f.size.height};
    std::vector<opensal::Rect> rects = first_ || damage_.empty() ? std::vector{whole} : damage_;
    // Many small rectangles cost more in headers than their bounds do in pixels.
    if (rects.size() > 8) {
        std::int64_t x0 = rects[0].x, y0 = rects[0].y, x1 = x0, y1 = y0;
        for (const auto& d : rects) {
            x0 = std::min<std::int64_t>(x0, d.x);
            y0 = std::min<std::int64_t>(y0, d.y);
            x1 = std::max<std::int64_t>(x1, std::int64_t(d.x) + d.width);
            y1 = std::max<std::int64_t>(y1, std::int64_t(d.y) + d.height);
        }
        rects = {opensal::Rect{std::int32_t(x0), std::int32_t(y0), std::uint32_t(x1 - x0), std::uint32_t(y1 - y0)}};
    }
    if (f.size != told_) {
        // The size it is drawn at: first, and whenever it changes.
        send_(opensal::wire::encode('s', std::format("{} {}", f.size.width, f.size.height)));
        told_ = f.size;
    }
    auto* px = shm_mem_.data;
    for (const auto& d : rects) {
        // XRGB's fourth byte is undefined; opensal's pixels are opaque there.
        if (f.format == kShmXrgb) {
            const auto x1 = std::min<std::uint64_t>(std::uint64_t(d.x) + d.width, f.size.width);
            const auto y1 = std::min<std::uint64_t>(std::uint64_t(d.y) + d.height, f.size.height);
            for (std::uint64_t y = std::uint64_t(std::max(0, d.y)); y < y1; ++y)
                for (std::uint64_t x = std::uint64_t(std::max(0, d.x)); x < x1; ++x)
                    px[y * f.stride + x * 4 + 3] = std::byte{0xff};
        }
        const opensal::Pixels surface{f.size, f.stride, std::span<const std::byte>(px, shm_mem_.size)};
        if (!send_rects_(o_.out, provided_, surface, std::span(&d, 1))) { provider_gone_ = true; break; }
    }
    // The mirrors: the same picture (all of it to one that has not had it).
    const opensal::Pixels surface{f.size, f.stride, std::span<const std::byte>(px, shm_mem_.size)};
    bool resized = false;
    for (auto& m : mirrors_) {
        if (!m.hello || m.gone) continue;
        if (m.told != f.size) {
            m.gone = !os::write_all(m.fd, opensal::wire::encode('s', std::format("{} {}", f.size.width, f.size.height)));
            m.told = f.size;
            resized = true;
        }
        const auto all = std::exchange(m.whole, false) ? std::vector{whole} : rects;
        if (!m.gone && !send_rects_(m.fd, m.provided, surface, all)) m.gone = true;
    }
    if (resized) tell_roles_();
    first_ = false;
    c_.send(frame_, op::frame_destroy, wl::Args{});
    frame_ = 0;
    capturing_ = false;
    offer_.reset();
    damage_.clear();
    last_frame_ = std::chrono::steady_clock::now();
}

void Bridge::key_(std::uint32_t code, bool down) {
    if (!vkbd_) return;
    if (down) held_.insert(code); else held_.erase(code);
    c_.send(vkbd_, op::vkbd_key, wl::Args{}.u(now_ms_()).u(code).u(down ? 1 : 0));
}

void Bridge::modifiers_() {
    if (vkbd_) c_.send(vkbd_, op::vkbd_modifiers, wl::Args{}.u(xkb_mods(mods_)).u(0).u(0).u(0));
}

void Bridge::input_(const opensal::Event& ev) {
    if (const auto* clip = std::get_if<opensal::ClipboardText>(&ev)) {
        set_selection_(clip->text);
        return;
    }
    if (const auto* k = std::get_if<opensal::Key>(&ev)) {
        if (k->repeat) return;                  // the compositor repeats a held key itself
        auto stroke = evdev_key(k->keysym);
        if (!stroke) return;
        if (const auto m = modifier_of(k->keysym)) {
            if (k->down) mods_ |= m; else mods_ &= ~m;
            key_(stroke->code, k->down);
            modifiers_();
            return;
        }
        // A character that needs Shift (one the window gave already shifted).
        const bool add_shift = stroke->shift && !(mods_ & opensal::Shift) && k->down;
        if (add_shift) { mods_ |= opensal::Shift; key_(42, true); modifiers_(); }
        key_(stroke->code, k->down);
        if (add_shift) { key_(stroke->code, false); mods_ &= ~opensal::Shift; key_(42, false); modifiers_(); }
    } else if (const auto* m = std::get_if<opensal::PointerMotion>(&ev)) {
        if (!vptr_ || output_size_.width == 0) return;
        const auto x = static_cast<std::uint32_t>(std::clamp(m->x, 0.0, double(output_size_.width - 1)));
        const auto y = static_cast<std::uint32_t>(std::clamp(m->y, 0.0, double(output_size_.height - 1)));
        c_.send(vptr_, op::vptr_motion_absolute, wl::Args{}.u(now_ms_()).u(x).u(y).u(output_size_.width).u(output_size_.height));
        c_.send(vptr_, op::vptr_frame, wl::Args{});
    } else if (const auto* b = std::get_if<opensal::PointerButton>(&ev)) {
        if (!vptr_) return;
        c_.send(vptr_, op::vptr_button, wl::Args{}.u(now_ms_()).u(evdev_button(b->button)).u(b->down ? 1 : 0));
        c_.send(vptr_, op::vptr_frame, wl::Args{});
    } else if (const auto* w = std::get_if<opensal::Wheel>(&ev)) {
        if (!vptr_) return;
        // A wheel step is 15 units of scroll (libinput's); up is negative.
        if (w->dy != 0) c_.send(vptr_, op::vptr_axis, wl::Args{}.u(now_ms_()).u(0).fixed(-w->dy * 15.0));
        if (w->dx != 0) c_.send(vptr_, op::vptr_axis, wl::Args{}.u(now_ms_()).u(1).fixed(w->dx * 15.0));
        c_.send(vptr_, op::vptr_frame, wl::Args{});
    } else if (const auto* r = std::get_if<opensal::Resized>(&ev)) {
        provider_own_ = r->size;
        if (!primary_mirror_()) resize_(r->size);
    } else if (const auto* f = std::get_if<opensal::Focus>(&ev)) {
        // Focus left the window: nothing it holds stays pressed here.
        if (!f->in) {
            for (auto code : std::vector(held_.begin(), held_.end())) key_(code, false);
            mods_ = 0;
            modifiers_();
        }
    }
}

// The provider as this session's shell: a program started in the session
// (its own process group, its output nowhere), a window brought forward.
void Bridge::shell_(const opensal::Event& ev) {
    if (!opensal::has(provided_, opensal::Interface::Shell)) return;
    if (const auto* l = std::get_if<opensal::Launch>(&ev)) {
        const auto app = std::ranges::find(apps_, l->app, &Application::id);
        if (app == apps_.end()) { log_(std::format("no application '{}' to start", l->app)); return; }
        // In a terminal: the default one, by the xdg convention (xdg-terminal-exec).
        auto argv = app->argv;
        if (app->terminal) argv.insert(argv.begin(), "xdg-terminal-exec");
        const auto home = o_.env.find("HOME");
        const int pid = os::spawn({.argv = argv, .env = o_.env,
                                   .cwd = home != o_.env.end() ? fs::path(home->second) : fs::path{}});
        if (pid < 0) log_(std::format("could not start {} ({})", app->id, argv[0]));
        else launched_.push_back(pid);
    } else if (const auto* a = std::get_if<opensal::Activate>(&ev)) {
        const auto w = windows_.find(a->window);
        if (w == windows_.end() || !seat_) return;
        if (w->second.minimized) c_.send(w->first, op::toplevel_unset_minimized, wl::Args{});
        c_.send(w->first, op::toplevel_activate, wl::Args{}.u(seat_));
    }
}

void Bridge::tell_windows_() {
    windows_changed_ = false;
    if (!opensal::has(provided_, opensal::Interface::Shell)) return;
    std::vector<opensal::Window> all;
    for (const auto& [handle, w] : windows_) all.push_back(w);
    if (!tell_(opensal::wire::encode('w', opensal::wire::encode_windows(all)))) provider_gone_ = true;
}

// What it can start: read again every few seconds (something installed
// while it runs), told when it changed.
void Bridge::tell_applications_() {
    apps_read_ = std::chrono::steady_clock::now();
    auto now = applications(application_dirs(o_.env));
    const auto same = [](const Application& a, const Application& b) {
        return a.id == b.id && a.name == b.name && a.argv == b.argv && a.terminal == b.terminal;
    };
    if (apps_told_ && std::ranges::equal(now, apps_, same)) return;
    apps_ = std::move(now);
    if (!opensal::has(provided_, opensal::Interface::Shell)) return;
    std::vector<opensal::Application> listed;
    for (const auto& a : apps_) listed.push_back({a.id, a.name});
    apps_told_ = tell_(opensal::wire::encode('a', opensal::wire::encode_applications(listed)));
    if (!apps_told_) provider_gone_ = true;
}

// The desktop's selection, read when it is text and not this bridge's own
// (its end: the main loop, reading_), then told the window ('b').
void Bridge::selection_(std::uint32_t offer) {
    // The offers before it are done with.
    for (auto it = offers_.begin(); it != offers_.end();) {
        if (it->first == offer) { ++it; continue; }
        c_.send(it->first, dc::offer_destroy, wl::Args{});
        it = offers_.erase(it);
    }
    if (!offer || !offers_.contains(offer)) return;
    const auto& mimes = offers_[offer];
    if (std::ranges::find(mimes, dc::kOwn) != mimes.end()) return;
    std::string mime;
    for (const auto* want : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"})
        if (std::ranges::find(mimes, std::string_view(want)) != mimes.end()) { mime = want; break; }
    if (mime.empty()) return;
    auto ends = os::pipe();
    if (!ends) return;
    if (reading_ >= 0) os::close(reading_);
    c_.send(offer, dc::offer_receive, wl::Args{}.str(mime).fd((*ends)[1]));
    os::close((*ends)[1]);
    reading_ = (*ends)[0];
    read_.clear();
}

// The window's clipboard, the desktop's selection (offered as text).
void Bridge::set_selection_(const std::string& text) {
    if (!dc_manager_ || !dc_device_ || text == selection_set_ || text == selection_told_) return;
    if (dc_source_) c_.send(dc_source_, dc::source_destroy, wl::Args{});
    dc_source_ = c_.new_id();
    c_.send(dc_manager_, dc::manager_create_source, wl::Args{}.u(dc_source_));
    for (const auto* mime : {"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "TEXT", "STRING"})
        c_.send(dc_source_, dc::source_offer, wl::Args{}.str(mime));
    c_.send(dc_source_, dc::source_offer, wl::Args{}.str(dc::kOwn));
    c_.send(dc_device_, dc::device_set_selection, wl::Args{}.u(dc_source_));
    selection_set_ = text;
}

void Bridge::accept_mirror_() {
    const int fd = os::accept(displays_);
    if (fd < 0) return;
    std::uint32_t n = 2;
    while (std::ranges::any_of(mirrors_, [&](const Mirror& m) { return m.number == n; })) ++n;
    auto& m = mirrors_.emplace_back();
    m.fd = fd;
    m.number = n;
    log_(std::format("display {} joined (a mirror of display 1)", n));
    using I = opensal::Interface;
    opensal::Interfaces wanted = I::Surface | I::Mode;
    if (vkbd_) wanted |= static_cast<opensal::Interfaces>(I::Keyboard);
    if (vptr_) wanted |= static_cast<opensal::Interfaces>(I::Pointer);
    if (dc_device_) wanted |= static_cast<opensal::Interfaces>(I::Clipboard);
    if (region_) wanted |= static_cast<opensal::Interfaces>(I::Region);
    if (host_cursor_ || cursor_session_) wanted |= I::Cursor | I::CursorImage;
    if (o_.shell) wanted |= static_cast<opensal::Interfaces>(I::Shell);
    if (!os::write_all(fd, opensal::wire::encode('h', std::to_string(wanted)))) m.gone = true;
}

// A mirror's stream: its hello (then what it is told, and what the others
// have), and its input -- the desktop's, but its size, which is its own.
bool Bridge::mirror_(Mirror& m, std::string_view bytes) {
    m.decoder.feed(bytes);
    for (;;) {
        auto next = m.decoder.next();
        if (!next) return false;
        if (!*next) return true;
        const auto& msg = **next;
        if (msg.kind == 'H') {
            std::istringstream in(msg.payload);
            in >> m.provided;
            // "<bits> <w> <h> <scale> <paced> [primary]": one that comes to show
            // the desktop at its own size is the primary (opensal 0.7).
            std::uint32_t w = 0, h = 0;
            double scale = 1;
            int paced = 0;
            std::string asked;
            if (in >> w >> h) m.own = {w, h};
            if (in >> scale >> paced >> asked && asked == "primary" && w > 0 && h > 0) {
                m.primary = true;
                log_(std::format("display {} is the primary (it asked)", m.number));
                resize_(m.own);
            }
            m.hello = true;
            m.whole = true;
            tell_roles_();
            if (!cursor_sent_.empty())
                (void)os::write_all(m.fd, cursor_sent_ == "none" ? opensal::wire::encode('c', "none") : cursor_sent_);
            windows_changed_ = true;
            apps_told_ = false;
            if (o_.shell) tell_applications_();
        } else if (msg.kind == 'E') {
            auto e = opensal::wire::decode_event(msg.payload);
            if (!e) continue;
            // A mirror's size is its own business; the primary's is the output's.
            if (const auto* r = std::get_if<opensal::Resized>(&*e)) {
                m.own = r->size;
                if (m.primary && primary_mirror_() == &m) resize_(r->size);
                continue;
            }
            if (std::holds_alternative<opensal::DensityChanged>(*e)) continue;
            if (std::holds_alternative<opensal::Launch>(*e) || std::holds_alternative<opensal::Activate>(*e)) shell_(*e);
            else input_(*e);
        }
    }
}

bool Bridge::send_rects_(int fd, opensal::Interfaces provided, const opensal::Pixels& surface, std::span<const opensal::Rect> rects) {
    for (const auto& d : rects) {
        if (d.width == 0 || d.height == 0) continue;
        const std::size_t row = std::size_t(d.width) * 4, rows = row * d.height;
        if (region_ && opensal::has(provided, opensal::Interface::Region) && rows <= region_->size) {
            if (region_at_ + rows > region_->size) region_at_ = 0;
            auto* dst = reinterpret_cast<std::byte*>(region_->data) + region_at_;
            for (std::uint32_t y = 0; y < d.height; ++y)
                std::memcpy(dst + std::size_t(y) * row, surface.bytes.data() + (std::size_t(d.y) + y) * surface.stride + std::size_t(d.x) * 4, row);
            const auto msg = opensal::wire::encode_region_frame({.rect = d, .stride = static_cast<std::uint32_t>(row), .offset = region_at_});
            region_at_ += rows;
            if (!os::write_all(fd, msg)) return false;
            continue;
        }
        // A window across a narrow channel takes the rows compressed (opensal 0.6).
        auto frame = opensal::wire::encode_frame(surface, d);
        if (opensal::has(provided, opensal::Interface::Compressed))
            if (auto z = opensal::wire::encode_compressed(std::string_view(frame).substr(5))) frame = opensal::wire::encode('z', *z);
        if (!os::write_all(fd, frame)) return false;
    }
    return true;
}

// Each window what it is ('d', to those that take it).
void Bridge::tell_roles_() {
    // The primary: a window that asked to be (the last one), else the first.
    const auto* p = primary_mirror_();
    const std::uint32_t primary = p ? p->number : 1;
    const auto role = [&](std::uint32_t n) {
        return n == primary ? opensal::DisplayRole{.number = n}
                            : opensal::DisplayRole{.number = n, .kind = opensal::DisplayRole::Kind::mirror, .of = primary, .size = told_};
    };
    if (opensal::has(provided_, opensal::Interface::Displays) && !mirrors_.empty() && (primary == 1 || told_.width > 0))
        (void)send_(opensal::wire::encode('d', opensal::wire::encode_display(role(1))));
    for (auto& m : mirrors_) {
        if (!m.hello || m.gone || !opensal::has(m.provided, opensal::Interface::Displays) || told_.width == 0) continue;
        if (!os::write_all(m.fd, opensal::wire::encode('d', opensal::wire::encode_display(role(m.number))))) m.gone = true;
    }
}

bool Bridge::provider_(std::string_view bytes) {
    decoder_.feed(bytes);
    for (;;) {
        auto m = decoder_.next();
        if (!m) { log_("the provider's stream is not opensal.wire"); return false; }
        if (!*m) return true;
        const auto& msg = **m;
        if (msg.kind == 'H') {
            // "<bits> <w> <h> <scale>"
            std::istringstream in(msg.payload);
            std::uint32_t bits = 0, w = 0, h = 0;
            if (in >> bits >> w >> h) {
                provider_own_ = {w, h};
                if (!primary_mirror_()) resize_({w, h});
            }
            provided_ = bits;
            if (!host_cursor_ && !cursor_session_ && opensal::has(provided_, opensal::Interface::CursorImage) && sources_
                && copy_capture_ && seat_)
                cursor_start_();
            windows_changed_ = true;
            apps_told_ = false;
            if (o_.shell) tell_applications_();
            tell_roles_();
        } else if (msg.kind == 'E') {
            if (auto e = opensal::wire::decode_event(msg.payload)) {
                if (std::holds_alternative<opensal::Launch>(*e) || std::holds_alternative<opensal::Activate>(*e)) shell_(*e);
                else input_(*e);
            }
        }
    }
}

int Bridge::run() {
    registry_ = c_.new_id();
    c_.send(wl::kDisplay, op::display_get_registry, wl::Args{}.u(registry_));
    if (!roundtrip_()) { log_("the compositor did not answer"); return 2; }
    std::uint32_t v = 0;
    shm_ = bind_("wl_shm", 1, v);
    seat_ = bind_("wl_seat", 1, v);
    output_ = bind_("wl_output", 2, v);
    screencopy_ = bind_("zwlr_screencopy_manager_v1", 3, screencopy_version_);
    vkbd_manager_ = bind_("zwp_virtual_keyboard_manager_v1", 1, v);
    std::uint32_t vptr_version = 0;
    vptr_manager_ = bind_("zwlr_virtual_pointer_manager_v1", 2, vptr_version);
    outman_ = bind_("zwlr_output_manager_v1", 2, v);
    dc_manager_ = bind_("zwlr_data_control_manager_v1", 2, v);
    if (o_.shell) toplevels_ = bind_("zwlr_foreign_toplevel_manager_v1", 3, v);
    sources_ = bind_("ext_output_image_capture_source_manager_v1", 1, v);
    copy_capture_ = bind_("ext_image_copy_capture_manager_v1", 1, v);
    if (!shm_ || !output_ || !screencopy_) {
        log_("the compositor has no screencopy (zwlr_screencopy_manager_v1) or no output: not a desktop opensal-wlr can show");
        return 1;
    }
    if (vkbd_manager_ && seat_) {
        vkbd_ = c_.new_id();
        c_.send(vkbd_manager_, op::vkbd_manager_create, wl::Args{}.u(seat_).u(vkbd_));
        std::string keymap(kKeymap);
        keymap.push_back('\0');
        if (auto m = os::shared_memory(keymap.size())) {
            std::memcpy(m->data, keymap.data(), keymap.size());
            c_.send(vkbd_, op::vkbd_keymap, wl::Args{}.u(1).fd(m->fd).u(std::uint32_t(keymap.size())));
            os::release(*m);
        }
    } else {
        log_("no virtual keyboard: the desktop is shown without keys");
    }
    if (vptr_manager_) {
        vptr_ = c_.new_id();
        if (vptr_version >= 2) c_.send(vptr_manager_, op::vptr_manager_create_with_output, wl::Args{}.u(seat_).u(output_).u(vptr_));
        else c_.send(vptr_manager_, op::vptr_manager_create, wl::Args{}.u(seat_).u(vptr_));
    }
    if (dc_manager_ && seat_) {
        dc_device_ = c_.new_id();
        c_.send(dc_manager_, dc::manager_get_device, wl::Args{}.u(dc_device_).u(seat_));
    }
    if (!roundtrip_()) return 1;
    // A region the windows map too (a machine's frames: opensal 0.7 Region).
    if (const auto env = os::environment(); env.contains("OPENSAL_REGION") && env.contains("OPENSAL_REGION_SIZE")) {
        const auto size = static_cast<std::size_t>(std::strtoull(env.at("OPENSAL_REGION_SIZE").c_str(), nullptr, 10));
        if (size >= (1u << 20)) region_ = os::map_file(env.at("OPENSAL_REGION"), size);
        log_(region_ ? std::format("frames go through a shared region ({} MiB)", size >> 20)
                     : std::string("the shared region for frames could not be mapped: frames go on the channel"));
    }
    using I = opensal::Interface;
    opensal::Interfaces wanted = I::Surface | I::Mode;
    if (region_) wanted |= static_cast<opensal::Interfaces>(I::Region);
    if (vkbd_) wanted |= static_cast<opensal::Interfaces>(I::Keyboard);
    if (vptr_) wanted |= static_cast<opensal::Interfaces>(I::Pointer);
    if (dc_device_) wanted |= static_cast<opensal::Interfaces>(I::Clipboard);
    // The pointer apart from the frames: the provider draws it.
    if (sources_ && copy_capture_ && seat_) wanted |= I::Cursor | I::CursorImage;
    if (o_.shell) {
        // Launching needs nothing of the compositor; listing its windows does.
        wanted |= static_cast<opensal::Interfaces>(I::Shell);
        if (!toplevels_) log_("the compositor does not list its windows (zwlr_foreign_toplevel_manager_v1): the shell starts programs only");
    }
    if (!send_(opensal::wire::encode('h', std::to_string(wanted)))) return 0;

    const auto period = std::chrono::milliseconds(1000 / std::max(1, o_.max_fps));
    std::array<char, 65536> buf{};
    if (!o_.displays.empty()) {
        displays_ = os::listen_unix(o_.displays);
        if (displays_ < 0) log_("no displays socket at " + o_.displays.string() + " (one window only)");
    }
    struct Closer {
        Bridge& b;
        ~Closer() {
            for (auto& m : b.mirrors_) os::close(m.fd);
            if (b.displays_ >= 0) {
                os::close(b.displays_);
                std::error_code ec;
                fs::remove(b.o_.displays, ec);
            }
        }
    } closer{*this};
    while (!provider_gone_) {
        if (o_.alive && !o_.alive()) { log_("the compositor ended"); return 1; }
        const auto now = std::chrono::steady_clock::now();
        if (!capturing_ && now >= last_frame_ + period && now >= retry_at_) capture_();
        std::vector<os::PollFd> p{{.fd = o_.in}, {.fd = c_.fd()}, {.fd = reading_}};
        if (displays_ >= 0) p.push_back({.fd = displays_});
        for (const auto& m : mirrors_) p.push_back({.fd = m.fd});
        const auto wait = capturing_ ? 500 : static_cast<int>(std::max<std::int64_t>(1,
            std::chrono::duration_cast<std::chrono::milliseconds>(last_frame_ + period - now).count()));
        os::poll(p, std::min(wait, 500));
        // The desktop's selection, as it comes; told when it has all come.
        if (reading_ >= 0 && (p[2].readable || p[2].closed)) {
            const auto n = os::read_some(reading_, buf);
            if (n && *n > 0 && read_.size() < (1u << 20)) read_.append(buf.data(), *n);
            else {
                os::close(reading_);
                reading_ = -1;
                if (!read_.empty() && read_ != selection_set_ && read_ != selection_told_) {
                    selection_told_ = read_;
                    if (!tell_(opensal::wire::encode('b', read_))) provider_gone_ = true;
                }
            }
        }
        std::size_t at = 3;
        if (displays_ >= 0 && (p[at++].readable)) accept_mirror_();
        for (auto& m : mirrors_) {
            if (at >= p.size()) break;
            const auto& q = p[at++];
            if (!q.readable && !q.closed) continue;
            auto n = os::read_some(m.fd, buf);
            if (!n || *n == 0 || !mirror_(m, std::string_view(buf.data(), *n))) m.gone = true;
        }
        const bool had_primary = primary_mirror_() != nullptr;
        bool left = false;
        for (auto it = mirrors_.begin(); it != mirrors_.end();)
            if (it->gone) { log_(std::format("display {} left", it->number)); os::close(it->fd); it = mirrors_.erase(it); left = true; }
            else ++it;
        // The primary that came went: the output is the first window's size again.
        if (left && had_primary) {
            if (const auto* p = primary_mirror_()) resize_(p->own);
            else if (provider_own_.width > 0) resize_(provider_own_);
            tell_roles_();
        }
        if (p[0].readable || p[0].closed) {
            auto n = os::read_some(o_.in, buf);
            if (!n || *n == 0) return 0;
            if (!provider_(std::string_view(buf.data(), *n))) return 0;
        }
        if (p[1].readable || p[1].closed) {
            auto events = c_.poll(0);
            if (!events) { log_("the compositor closed its socket"); return 1; }
            for (const auto& e : *events) handle_(e);
        }
        if (windows_changed_) tell_windows_();
        if (o_.shell && apps_told_ && std::chrono::steady_clock::now() - apps_read_ > std::chrono::seconds(5)) tell_applications_();
        std::erase_if(launched_, [](int pid) { return os::exited(pid).has_value(); });
    }
    return 0;
}

}  // namespace

int bridge(const Options& o) {
    // A peer that has gone (the window, the compositor) is a write that
    // fails, not a signal that ends this process before it stops its
    // compositor. Set here, after the compositor was started: it is inherited.
    os::ignore_broken_pipes();
    auto c = wl::Connection::connect(o.socket);
    if (!c) { if (o.log) o.log(c.error()); return 2; }
    Bridge b(o, std::move(*c));
    return b.run();
}

}  // namespace opensal::wlr
