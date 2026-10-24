// xdesk.scenes: the wallpaper engine's scenes (Luban OS design 2026-10-24
// §9.7) -- classic things of mathematics, physics and computer graphics,
// computed on the CPU at a small size and shown larger: Gray-Scott
// reaction-diffusion (the default: slow, quiet), a Mandelbrot zoom, a Julia
// set that turns, a Lorenz attractor, a Perlin flow field, boids, Conway's
// life, double pendulums, n bodies, stable fluids (Stam), a ray-marched
// signed-distance scene. Each steps by time, draws into ARGB pixels, and
// answers the pointer where it makes sense (a drop of reagent, dye in the
// fluid, boids that keep away, cells born). Deterministic from its seed.
export module xdesk.scenes;

import std;

export namespace xdesk::scenes {

class Scene {
public:
    virtual ~Scene() = default;
    // Its own size (the small one it computes at).
    virtual void resize(int width, int height) = 0;
    // `dt` seconds of its time.
    virtual void step(double dt) = 0;
    // Its picture: 0xAARRGGBB, `width` x `height` (its size).
    virtual void draw(std::span<std::uint32_t> pixels) const = 0;
    // The pointer at (x, y) in its pixels; `down`: a button held.
    virtual void pointer(double x, double y, bool down) { (void)x; (void)y; (void)down; }
    // Where the view is (the desktop's camera): a scene that follows it pans.
    virtual void camera(double x, double y, double zoom) { (void)x; (void)y; (void)zoom; }
};

// Every scene by name, the default first.
std::span<const std::string_view> names();
// The scene `name` (an unknown name: the default), from `seed`.
std::unique_ptr<Scene> make(std::string_view name, std::uint32_t seed = 1);

}  // namespace xdesk::scenes
