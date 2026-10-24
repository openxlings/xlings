module xdesk.scenes;

import std;

namespace xdesk::scenes {

namespace {

constexpr double kPi = 3.14159265358979323846;

std::uint32_t argb_(double r, double g, double b) {
    auto c = [](double v) { return static_cast<std::uint32_t>(std::clamp(v, 0.0, 1.0) * 255.0 + 0.5); };
    return 0xff000000u | (c(r) << 16) | (c(g) << 8) | c(b);
}

// A cosine palette (Inigo Quilez): a + b cos(2 pi (c t + d)), per channel.
struct Palette {
    std::array<double, 3> a, b, c, d;
    std::uint32_t at(double t) const {
        double v[3];
        for (int k = 0; k < 3; ++k) v[k] = a[k] + b[k] * std::cos(2 * kPi * (c[k] * t + d[k]));
        return argb_(v[0], v[1], v[2]);
    }
};
constexpr Palette kDeep{{0.10, 0.12, 0.18}, {0.12, 0.16, 0.22}, {1.0, 1.0, 1.0}, {0.00, 0.10, 0.20}};
constexpr Palette kEmber{{0.50, 0.30, 0.20}, {0.50, 0.30, 0.25}, {1.0, 1.0, 0.5}, {0.80, 0.90, 0.30}};
constexpr Palette kOcean{{0.25, 0.40, 0.55}, {0.25, 0.35, 0.40}, {1.0, 1.0, 1.0}, {0.30, 0.20, 0.20}};

// A trail picture: light added where things pass, fading every step.
struct Trails {
    int w { 0 }, h { 0 };
    std::vector<float> r, g, b;
    void resize(int width, int height) {
        w = width; h = height;
        r.assign(static_cast<std::size_t>(w) * h, 0); g = r; b = r;
    }
    void fade(float keep) {
        for (std::size_t k = 0; k < r.size(); ++k) { r[k] *= keep; g[k] *= keep; b[k] *= keep; }
    }
    void add(double x, double y, float cr, float cg, float cb) {
        const int ix = static_cast<int>(x), iy = static_cast<int>(y);
        if (ix < 0 || iy < 0 || ix >= w || iy >= h) return;
        const auto at = static_cast<std::size_t>(iy) * w + ix;
        r[at] += cr; g[at] += cg; b[at] += cb;
    }
    void draw(std::span<std::uint32_t> px, std::array<double, 3> base) const {
        for (std::size_t k = 0; k < r.size() && k < px.size(); ++k)
            px[k] = argb_(base[0] + 1 - std::exp(-r[k]), base[1] + 1 - std::exp(-g[k]), base[2] + 1 - std::exp(-b[k]));
    }
};

// ── Gray-Scott reaction-diffusion ─────────────────────────────────────

class ReactionDiffusion final : public Scene {
public:
    explicit ReactionDiffusion(std::uint32_t seed) : rng_(seed) {}
    void resize(int w, int h) override {
        w_ = std::max(8, w); h_ = std::max(8, h);
        u_.assign(static_cast<std::size_t>(w_) * h_, 1.0f);
        v_.assign(u_.size(), 0.0f);
        std::uniform_int_distribution<int> x(0, w_ - 1), y(0, h_ - 1);
        for (int k = 0; k < 24; ++k) drop_(x(rng_), y(rng_), 3);
    }
    void step(double dt) override {
        int n = std::clamp(static_cast<int>(dt * 240), 1, 24);
        while (n--) iterate_();
    }
    void draw(std::span<std::uint32_t> px) const override {
        for (std::size_t k = 0; k < u_.size() && k < px.size(); ++k) px[k] = kDeep.at(std::clamp(double(u_[k] - v_[k]), 0.0, 1.0) * 0.8);
    }
    void pointer(double x, double y, bool down) override { if (down) drop_(static_cast<int>(x), static_cast<int>(y), 2); }
private:
    void drop_(int cx, int cy, int r) {
        for (int y = -r; y <= r; ++y)
            for (int x = -r; x <= r; ++x) {
                const int px = (cx + x + w_) % w_, py = (cy + y + h_) % h_;
                v_[static_cast<std::size_t>(py) * w_ + px] = 1.0f;
            }
    }
    void iterate_() {
        constexpr float Du = 1.0f, Dv = 0.5f, F = 0.0367f, K = 0.0649f;
        auto un = u_, vn = v_;
        for (int y = 0; y < h_; ++y) {
            const int ym = (y + h_ - 1) % h_, yp = (y + 1) % h_;
            for (int x = 0; x < w_; ++x) {
                const int xm = (x + w_ - 1) % w_, xp = (x + 1) % w_;
                const auto at = [&](int xx, int yy) { return static_cast<std::size_t>(yy) * w_ + xx; };
                const auto c = at(x, y);
                const float lu = u_[at(xm, y)] + u_[at(xp, y)] + u_[at(x, ym)] + u_[at(x, yp)] - 4 * u_[c];
                const float lv = v_[at(xm, y)] + v_[at(xp, y)] + v_[at(x, ym)] + v_[at(x, yp)] - 4 * v_[c];
                const float uvv = u_[c] * v_[c] * v_[c];
                un[c] = std::clamp(u_[c] + Du * 0.2f * lu - uvv + F * (1 - u_[c]), 0.0f, 1.0f);
                vn[c] = std::clamp(v_[c] + Dv * 0.2f * lv + uvv - (F + K) * v_[c], 0.0f, 1.0f);
            }
        }
        u_.swap(un);
        v_.swap(vn);
    }
    std::mt19937 rng_;
    int w_ { 0 }, h_ { 0 };
    std::vector<float> u_, v_;
};

// ── escape-time fractals: a Mandelbrot zoom, a Julia set that turns ───

class Fractal final : public Scene {
public:
    explicit Fractal(bool julia) : julia_(julia) {}
    void resize(int w, int h) override {
        w_ = std::max(8, w); h_ = std::max(8, h);
        n_.assign(static_cast<std::size_t>(w_) * h_, 0.0f);
        row_ = 0;
    }
    void step(double dt) override {
        t_ += dt;
        // A band of rows each step: the whole picture in about a second.
        const int rows = std::max(1, static_cast<int>(h_ * std::min(1.0, dt * 2)));
        for (int k = 0; k < rows; ++k) { row_ = (row_ + 1) % h_; render_row_(row_); }
    }
    void draw(std::span<std::uint32_t> px) const override {
        for (std::size_t k = 0; k < n_.size() && k < px.size(); ++k)
            px[k] = n_[k] < 0 ? 0xff07080cu : (julia_ ? kOcean : kEmber).at(std::fmod(n_[k] * 0.02, 1.0));
    }
    void camera(double x, double y, double zoom) override { pan_x_ = x / 4000; pan_y_ = y / 4000; cam_zoom_ = std::max(0.1, zoom); }
private:
    void render_row_(int y) {
        const double zoom = julia_ ? cam_zoom_ : std::exp(std::fmod(t_ * 0.06, 28.0)) * cam_zoom_;
        const double cx = julia_ ? pan_x_ : -0.743643887037151 + pan_x_ / zoom, cy = julia_ ? pan_y_ : 0.131825904205330 + pan_y_ / zoom;
        const double span = 3.2 / zoom, aspect = double(h_) / w_;
        const int limit = static_cast<int>(80 + 30 * std::log2(std::max(1.0, zoom)));
        const double jr = 0.7885 * std::cos(t_ * 0.05), ji = 0.7885 * std::sin(t_ * 0.05);
        for (int x = 0; x < w_; ++x) {
            const double fx = cx + (x / double(w_) - 0.5) * span, fy = cy + (y / double(h_) - 0.5) * span * aspect;
            double zr = julia_ ? fx : 0, zi = julia_ ? fy : 0;
            const double cr = julia_ ? jr : fx, ci = julia_ ? ji : fy;
            int i = 0;
            for (; i < limit && zr * zr + zi * zi < 256; ++i) {
                const double t = zr * zr - zi * zi + cr;
                zi = 2 * zr * zi + ci;
                zr = t;
            }
            // Smooth: the fraction past the last whole step.
            n_[static_cast<std::size_t>(y) * w_ + x] = i >= limit ? -1.0f : static_cast<float>(i + 1 - std::log2(std::log2(zr * zr + zi * zi) / 2));
        }
    }
    bool julia_;
    int w_ { 0 }, h_ { 0 }, row_ { 0 };
    double t_ { 0 }, pan_x_ { 0 }, pan_y_ { 0 }, cam_zoom_ { 1 };
    std::vector<float> n_;
};

// ── the Lorenz attractor ─────────────────────────────────────────────

class Lorenz final : public Scene {
public:
    explicit Lorenz(std::uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> d(-1, 1);
        for (auto& p : points_) p = {1 + d(rng), 1 + d(rng), 20 + d(rng)};
    }
    void resize(int w, int h) override { trails_.resize(std::max(8, w), std::max(8, h)); }
    void step(double dt) override {
        trails_.fade(static_cast<float>(std::pow(0.4, dt)));
        constexpr double s = 10, r = 28, b = 8.0 / 3;
        const int sub = std::clamp(static_cast<int>(dt * 2000), 1, 400);
        for (std::size_t k = 0; k < points_.size(); ++k) {
            auto& [x, y, z] = points_[k];
            for (int i = 0; i < sub; ++i) {
                const double h = 0.0025;
                const double dx = s * (y - x), dy = x * (r - z) - y, dz = x * y - b * z;
                x += dx * h; y += dy * h; z += dz * h;
                const double sx = (x / 50 + 0.5) * trails_.w, sy = (1 - z / 55) * trails_.h;
                const float t = static_cast<float>(k) / points_.size();
                trails_.add(sx, sy, 0.02f + 0.03f * t, 0.03f, 0.05f - 0.02f * t);
            }
        }
    }
    void draw(std::span<std::uint32_t> px) const override { trails_.draw(px, {0.03, 0.035, 0.05}); }
private:
    std::array<std::array<double, 3>, 6> points_ {};
    Trails trails_;
};

// ── a Perlin flow field ──────────────────────────────────────────────

double fade_(double t) { return t * t * t * (t * (t * 6 - 15) + 10); }

class Noise {
public:
    explicit Noise(std::uint32_t seed) {
        std::iota(p_.begin(), p_.begin() + 256, 0);
        std::mt19937 rng(seed);
        std::shuffle(p_.begin(), p_.begin() + 256, rng);
        for (int k = 0; k < 256; ++k) p_[256 + k] = p_[k];
    }
    double at(double x, double y, double z) const {
        const int X = static_cast<int>(std::floor(x)) & 255, Y = static_cast<int>(std::floor(y)) & 255, Z = static_cast<int>(std::floor(z)) & 255;
        x -= std::floor(x); y -= std::floor(y); z -= std::floor(z);
        const double u = fade_(x), v = fade_(y), w = fade_(z);
        auto grad = [](int h, double gx, double gy, double gz) {
            const int g = h & 15;
            const double a = g < 8 ? gx : gy, b = g < 4 ? gy : (g == 12 || g == 14 ? gx : gz);
            return ((g & 1) ? -a : a) + ((g & 2) ? -b : b);
        };
        const int A = p_[X] + Y, AA = p_[A] + Z, AB = p_[A + 1] + Z, B = p_[X + 1] + Y, BA = p_[B] + Z, BB = p_[B + 1] + Z;
        auto lerp = [](double t, double a, double b) { return a + t * (b - a); };
        return lerp(w, lerp(v, lerp(u, grad(p_[AA], x, y, z), grad(p_[BA], x - 1, y, z)), lerp(u, grad(p_[AB], x, y - 1, z), grad(p_[BB], x - 1, y - 1, z))),
                    lerp(v, lerp(u, grad(p_[AA + 1], x, y, z - 1), grad(p_[BA + 1], x - 1, y, z - 1)),
                         lerp(u, grad(p_[AB + 1], x, y - 1, z - 1), grad(p_[BB + 1], x - 1, y - 1, z - 1))));
    }
private:
    std::array<int, 512> p_ {};
};

class FlowField final : public Scene {
public:
    explicit FlowField(std::uint32_t seed) : noise_(seed), rng_(seed) {}
    void resize(int w, int h) override {
        trails_.resize(std::max(8, w), std::max(8, h));
        std::uniform_real_distribution<double> x(0, trails_.w), y(0, trails_.h);
        particles_.resize(2400);
        for (auto& p : particles_) p = {x(rng_), y(rng_)};
    }
    void step(double dt) override {
        t_ += dt * 0.05;
        trails_.fade(static_cast<float>(std::pow(0.5, dt)));
        std::uniform_real_distribution<double> x(0, trails_.w), y(0, trails_.h);
        for (auto& [px, py] : particles_) {
            const double a = noise_.at(px * 0.012, py * 0.012, t_) * 4 * kPi;
            double vx = std::cos(a), vy = std::sin(a);
            // Away from the pointer.
            const double dx = px - pointer_x_, dy = py - pointer_y_, d2 = dx * dx + dy * dy;
            if (d2 < 900 && d2 > 0.01) { vx += dx / std::sqrt(d2) * 2; vy += dy / std::sqrt(d2) * 2; }
            px += vx * dt * 30;
            py += vy * dt * 30;
            if (px < 0 || py < 0 || px >= trails_.w || py >= trails_.h) { px = x(rng_); py = y(rng_); }
            trails_.add(px, py, 0.05f, 0.09f, 0.12f);
        }
    }
    void draw(std::span<std::uint32_t> px) const override { trails_.draw(px, {0.04, 0.05, 0.07}); }
    void pointer(double x, double y, bool) override { pointer_x_ = x; pointer_y_ = y; }
private:
    Noise noise_;
    std::mt19937 rng_;
    Trails trails_;
    std::vector<std::pair<double, double>> particles_;
    double t_ { 0 }, pointer_x_ { -1e9 }, pointer_y_ { -1e9 };
};

// ── boids ────────────────────────────────────────────────────────────

class Boids final : public Scene {
public:
    explicit Boids(std::uint32_t seed) : rng_(seed) {}
    void resize(int w, int h) override {
        trails_.resize(std::max(8, w), std::max(8, h));
        std::uniform_real_distribution<double> x(0, trails_.w), y(0, trails_.h), v(-1, 1);
        boids_.resize(260);
        for (auto& b : boids_) b = {x(rng_), y(rng_), v(rng_) * 20, v(rng_) * 20};
    }
    void step(double dt) override {
        trails_.fade(static_cast<float>(std::pow(0.08, dt)));
        const double W = trails_.w, H = trails_.h;
        auto next = boids_;
        for (std::size_t i = 0; i < boids_.size(); ++i) {
            const auto& b = boids_[i];
            double cx = 0, cy = 0, ax = 0, ay = 0, sx = 0, sy = 0;
            int n = 0;
            for (std::size_t j = 0; j < boids_.size(); ++j) {
                if (i == j) continue;
                const double dx = boids_[j][0] - b[0], dy = boids_[j][1] - b[1], d2 = dx * dx + dy * dy;
                if (d2 > 400) continue;
                ++n;
                cx += boids_[j][0]; cy += boids_[j][1];
                ax += boids_[j][2]; ay += boids_[j][3];
                if (d2 < 36) { sx -= dx; sy -= dy; }
            }
            auto& o = next[i];
            if (n) {
                o[2] += ((cx / n - b[0]) * 0.6 + (ax / n - b[2]) * 0.4 + sx * 3) * dt;
                o[3] += ((cy / n - b[1]) * 0.6 + (ay / n - b[3]) * 0.4 + sy * 3) * dt;
            }
            const double dx = b[0] - pointer_x_, dy = b[1] - pointer_y_, d2 = dx * dx + dy * dy;
            if (d2 < 1600 && d2 > 0.01) { o[2] += dx / std::sqrt(d2) * 200 * dt; o[3] += dy / std::sqrt(d2) * 200 * dt; }
            const double speed = std::hypot(o[2], o[3]), cap = 40;
            if (speed > cap) { o[2] *= cap / speed; o[3] *= cap / speed; }
            if (speed < 10 && speed > 0) { o[2] *= 10 / speed; o[3] *= 10 / speed; }
            o[0] = std::fmod(o[0] + o[2] * dt + W, W);
            o[1] = std::fmod(o[1] + o[3] * dt + H, H);
            trails_.add(o[0], o[1], 0.5f, 0.45f, 0.3f);
        }
        boids_ = std::move(next);
    }
    void draw(std::span<std::uint32_t> px) const override { trails_.draw(px, {0.05, 0.05, 0.07}); }
    void pointer(double x, double y, bool) override { pointer_x_ = x; pointer_y_ = y; }
private:
    std::mt19937 rng_;
    Trails trails_;
    std::vector<std::array<double, 4>> boids_;
    double pointer_x_ { -1e9 }, pointer_y_ { -1e9 };
};

// ── Conway's life ────────────────────────────────────────────────────

class Life final : public Scene {
public:
    explicit Life(std::uint32_t seed) : rng_(seed) {}
    void resize(int w, int h) override {
        w_ = std::max(8, w / 2); h_ = std::max(8, h / 2);
        ow_ = w; oh_ = h;
        seed_();
    }
    void step(double dt) override {
        acc_ += dt;
        while (acc_ > 0.12) {
            acc_ -= 0.12;
            generation_();
        }
    }
    void draw(std::span<std::uint32_t> px) const override {
        for (int y = 0; y < oh_; ++y)
            for (int x = 0; x < ow_; ++x) {
                const auto at = static_cast<std::size_t>(y) * ow_ + x;
                if (at >= px.size()) return;
                const auto age = cells_[static_cast<std::size_t>(std::min(h_ - 1, y / 2)) * w_ + std::min(w_ - 1, x / 2)];
                px[at] = age ? kOcean.at(0.2 + std::min(age, std::uint8_t(40)) * 0.01) : 0xff0b0d12u;
            }
    }
    void pointer(double x, double y, bool down) override {
        if (!down) return;
        std::bernoulli_distribution live(0.5);
        for (int dy = -3; dy <= 3; ++dy)
            for (int dx = -3; dx <= 3; ++dx) {
                const int cx = (static_cast<int>(x / 2) + dx + w_) % w_, cy = (static_cast<int>(y / 2) + dy + h_) % h_;
                if (live(rng_)) cells_[static_cast<std::size_t>(cy) * w_ + cx] = 1;
            }
    }
private:
    void seed_() {
        cells_.assign(static_cast<std::size_t>(w_) * h_, 0);
        std::bernoulli_distribution live(0.22);
        for (auto& c : cells_) c = live(rng_) ? 1 : 0;
        still_ = 0;
    }
    void generation_() {
        auto next = cells_;
        std::size_t alive = 0;
        for (int y = 0; y < h_; ++y)
            for (int x = 0; x < w_; ++x) {
                int n = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if ((dx || dy) && cells_[static_cast<std::size_t>((y + dy + h_) % h_) * w_ + (x + dx + w_) % w_]) ++n;
                auto& c = next[static_cast<std::size_t>(y) * w_ + x];
                const bool was = cells_[static_cast<std::size_t>(y) * w_ + x] != 0;
                c = (was && (n == 2 || n == 3)) ? static_cast<std::uint8_t>(std::min(255, cells_[static_cast<std::size_t>(y) * w_ + x] + 1))
                    : (!was && n == 3) ? 1 : 0;
                alive += c ? 1 : 0;
            }
        cells_.swap(next);
        // Still, or nearly empty: sown again.
        still_ = alive == alive_ ? still_ + 1 : 0;
        alive_ = alive;
        if (still_ > 60 || alive < cells_.size() / 200) seed_();
    }
    std::mt19937 rng_;
    int w_ { 0 }, h_ { 0 }, ow_ { 0 }, oh_ { 0 }, still_ { 0 };
    std::size_t alive_ { 0 };
    double acc_ { 0 };
    std::vector<std::uint8_t> cells_;
};

// ── double pendulums ─────────────────────────────────────────────────

class Pendulums final : public Scene {
public:
    explicit Pendulums(std::uint32_t seed) {
        std::mt19937 rng(seed);
        std::uniform_real_distribution<double> e(-1e-3, 1e-3);
        for (std::size_t k = 0; k < state_.size(); ++k) state_[k] = {kPi * 0.75 + k * 1e-3 + e(rng), kPi * 0.9, 0, 0};
    }
    void resize(int w, int h) override { trails_.resize(std::max(8, w), std::max(8, h)); }
    void step(double dt) override {
        trails_.fade(static_cast<float>(std::pow(0.6, dt)));
        const int sub = std::clamp(static_cast<int>(dt * 600), 1, 120);
        for (std::size_t k = 0; k < state_.size(); ++k)
            for (int i = 0; i < sub; ++i) {
                rk4_(state_[k], 1.0 / 600);
                const auto& s = state_[k];
                const double L = std::min(trails_.w, trails_.h) * 0.22;
                const double x = trails_.w / 2.0 + L * (std::sin(s[0]) + std::sin(s[1]));
                const double y = trails_.h / 2.0 + L * (std::cos(s[0]) + std::cos(s[1]));
                const float t = static_cast<float>(k) / state_.size();
                trails_.add(x, y, 0.01f + 0.03f * t, 0.02f, 0.04f - 0.02f * t);
            }
    }
    void draw(std::span<std::uint32_t> px) const override { trails_.draw(px, {0.03, 0.03, 0.05}); }
private:
    using S = std::array<double, 4>;   // the two angles, their speeds
    static S deriv_(const S& s) {
        constexpr double g = 9.81;
        const double d = s[0] - s[1], den = 3 - std::cos(2 * d);
        const double a1 = (-3 * g * std::sin(s[0]) - g * std::sin(s[0] - 2 * s[1]) - 2 * std::sin(d) * (s[3] * s[3] + s[2] * s[2] * std::cos(d))) / den;
        const double a2 = (2 * std::sin(d) * (2 * s[2] * s[2] + 2 * g * std::cos(s[0]) + s[3] * s[3] * std::cos(d))) / den;
        return {s[2], s[3], a1, a2};
    }
    static void rk4_(S& s, double h) {
        auto add = [](const S& a, const S& b, double f) { S o; for (int k = 0; k < 4; ++k) o[k] = a[k] + b[k] * f; return o; };
        const auto k1 = deriv_(s), k2 = deriv_(add(s, k1, h / 2)), k3 = deriv_(add(s, k2, h / 2)), k4 = deriv_(add(s, k3, h));
        for (int k = 0; k < 4; ++k) s[k] += h / 6 * (k1[k] + 2 * k2[k] + 2 * k3[k] + k4[k]);
    }
    std::array<S, 6> state_ {};
    Trails trails_;
};

// ── n bodies ─────────────────────────────────────────────────────────

class Bodies final : public Scene {
public:
    explicit Bodies(std::uint32_t seed) : rng_(seed) {}
    void resize(int w, int h) override {
        trails_.resize(std::max(8, w), std::max(8, h));
        std::uniform_real_distribution<double> r(0.15, 0.45), a(0, 2 * kPi);
        bodies_.clear();
        for (int k = 0; k < 90; ++k) {
            const double rr = r(rng_) * std::min(w, h), aa = a(rng_);
            const double v = std::sqrt(4000 / rr);
            bodies_.push_back({w / 2.0 + rr * std::cos(aa), h / 2.0 + rr * std::sin(aa), -v * std::sin(aa), v * std::cos(aa)});
        }
    }
    void step(double dt) override {
        trails_.fade(static_cast<float>(std::pow(0.3, dt)));
        const int sub = std::clamp(static_cast<int>(dt * 120), 1, 20);
        const double h = dt / sub, cx = trails_.w / 2.0, cy = trails_.h / 2.0;
        for (int i = 0; i < sub; ++i) {
            for (auto& b : bodies_) {
                double ax = 0, ay = 0;
                const double dx0 = cx - b[0], dy0 = cy - b[1], r0 = dx0 * dx0 + dy0 * dy0 + 25;
                ax += 4000 * dx0 / (r0 * std::sqrt(r0)) * 1;
                ay += 4000 * dy0 / (r0 * std::sqrt(r0)) * 1;
                for (const auto& o : bodies_) {
                    const double dx = o[0] - b[0], dy = o[1] - b[1], r2 = dx * dx + dy * dy + 16;
                    ax += 8 * dx / (r2 * std::sqrt(r2));
                    ay += 8 * dy / (r2 * std::sqrt(r2));
                }
                b[2] += ax * h * 60;
                b[3] += ay * h * 60;
            }
            for (auto& b : bodies_) {
                b[0] += b[2] * h;
                b[1] += b[3] * h;
                trails_.add(b[0], b[1], 0.06f, 0.05f, 0.04f);
            }
        }
    }
    void draw(std::span<std::uint32_t> px) const override { trails_.draw(px, {0.02, 0.025, 0.04}); }
private:
    std::mt19937 rng_;
    Trails trails_;
    std::vector<std::array<double, 4>> bodies_;
};

// ── stable fluids (Stam) ─────────────────────────────────────────────

class Fluid final : public Scene {
public:
    explicit Fluid(std::uint32_t seed) : rng_(seed) {}
    void resize(int w, int h) override {
        ow_ = std::max(8, w); oh_ = std::max(8, h);
        n_ = std::max(16, w / 2); m_ = std::max(16, h / 2);
        const auto size = static_cast<std::size_t>(n_ + 2) * (m_ + 2);
        u_.assign(size, 0); v_ = u_; d_ = u_; u0_ = u_; v0_ = u_; d0_ = u_;
    }
    void step(double dt) override {
        dt = std::min(dt, 0.05);
        t_ += dt;
        // Quiet sources moving round, and what the pointer gave.
        const double sx = n_ / 2.0 + std::cos(t_ * 0.3) * n_ * 0.3, sy = m_ / 2.0 + std::sin(t_ * 0.21) * m_ * 0.3;
        splat_(sx, sy, std::cos(t_ * 0.7) * 30, std::sin(t_ * 0.5) * 30, 40 * dt);
        velocity_(dt);
        density_(dt);
        for (auto& d : d_) d *= static_cast<float>(std::pow(0.7, dt));
    }
    void draw(std::span<std::uint32_t> px) const override {
        for (int y = 0; y < oh_; ++y)
            for (int x = 0; x < ow_; ++x) {
                const auto at = static_cast<std::size_t>(y) * ow_ + x;
                if (at >= px.size()) return;
                const double d = d_[ix_(1 + x * n_ / ow_, 1 + y * m_ / oh_)];
                px[at] = kOcean.at(0.15 + std::min(1.0, d * 0.08) * 0.6);
            }
    }
    void pointer(double x, double y, bool down) override {
        const double fx = x * n_ / ow_, fy = y * m_ / oh_;
        if (down && have_last_) splat_(fx, fy, (fx - last_x_) * 60, (fy - last_y_) * 60, 30);
        last_x_ = fx; last_y_ = fy;
        have_last_ = down;
    }
private:
    std::size_t ix_(int x, int y) const { return static_cast<std::size_t>(std::clamp(y, 0, m_ + 1)) * (n_ + 2) + std::clamp(x, 0, n_ + 1); }
    void splat_(double x, double y, double vx, double vy, double amount) {
        for (int dy = -2; dy <= 2; ++dy)
            for (int dx = -2; dx <= 2; ++dx) {
                const auto at = ix_(static_cast<int>(x) + dx, static_cast<int>(y) + dy);
                u_[at] += static_cast<float>(vx * 0.1);
                v_[at] += static_cast<float>(vy * 0.1);
                d_[at] += static_cast<float>(amount * 0.2);
            }
    }
    void bounds_(int b, std::vector<float>& x) const {
        for (int i = 1; i <= m_; ++i) { x[ix_(0, i)] = b == 1 ? -x[ix_(1, i)] : x[ix_(1, i)]; x[ix_(n_ + 1, i)] = b == 1 ? -x[ix_(n_, i)] : x[ix_(n_, i)]; }
        for (int i = 1; i <= n_; ++i) { x[ix_(i, 0)] = b == 2 ? -x[ix_(i, 1)] : x[ix_(i, 1)]; x[ix_(i, m_ + 1)] = b == 2 ? -x[ix_(i, m_)] : x[ix_(i, m_)]; }
    }
    void solve_(int b, std::vector<float>& x, const std::vector<float>& x0, float a, float c) const {
        for (int k = 0; k < 12; ++k) {
            for (int j = 1; j <= m_; ++j)
                for (int i = 1; i <= n_; ++i)
                    x[ix_(i, j)] = (x0[ix_(i, j)] + a * (x[ix_(i - 1, j)] + x[ix_(i + 1, j)] + x[ix_(i, j - 1)] + x[ix_(i, j + 1)])) / c;
            bounds_(b, x);
        }
    }
    void advect_(int b, std::vector<float>& d, const std::vector<float>& d0, const std::vector<float>& u, const std::vector<float>& v, double dt) const {
        for (int j = 1; j <= m_; ++j)
            for (int i = 1; i <= n_; ++i) {
                double x = std::clamp(i - dt * 10 * u[ix_(i, j)], 0.5, n_ + 0.5), y = std::clamp(j - dt * 10 * v[ix_(i, j)], 0.5, m_ + 0.5);
                const int i0 = static_cast<int>(x), j0 = static_cast<int>(y);
                const double s1 = x - i0, s0 = 1 - s1, t1 = y - j0, t0 = 1 - t1;
                d[ix_(i, j)] = static_cast<float>(s0 * (t0 * d0[ix_(i0, j0)] + t1 * d0[ix_(i0, j0 + 1)]) + s1 * (t0 * d0[ix_(i0 + 1, j0)] + t1 * d0[ix_(i0 + 1, j0 + 1)]));
            }
        bounds_(b, d);
    }
    void project_(std::vector<float>& u, std::vector<float>& v, std::vector<float>& p, std::vector<float>& div) const {
        for (int j = 1; j <= m_; ++j)
            for (int i = 1; i <= n_; ++i) {
                div[ix_(i, j)] = -0.5f * (u[ix_(i + 1, j)] - u[ix_(i - 1, j)] + v[ix_(i, j + 1)] - v[ix_(i, j - 1)]);
                p[ix_(i, j)] = 0;
            }
        bounds_(0, div);
        bounds_(0, p);
        solve_(0, p, div, 1, 4);
        for (int j = 1; j <= m_; ++j)
            for (int i = 1; i <= n_; ++i) {
                u[ix_(i, j)] -= 0.5f * (p[ix_(i + 1, j)] - p[ix_(i - 1, j)]);
                v[ix_(i, j)] -= 0.5f * (p[ix_(i, j + 1)] - p[ix_(i, j - 1)]);
            }
        bounds_(1, u);
        bounds_(2, v);
    }
    void velocity_(double dt) {
        u0_ = u_; v0_ = v_;
        advect_(1, u_, u0_, u0_, v0_, dt);
        advect_(2, v_, v0_, u0_, v0_, dt);
        project_(u_, v_, u0_, v0_);
    }
    void density_(double dt) {
        d0_ = d_;
        advect_(0, d_, d0_, u_, v_, dt);
    }
    std::mt19937 rng_;
    int n_ { 0 }, m_ { 0 }, ow_ { 0 }, oh_ { 0 };
    double t_ { 0 }, last_x_ { 0 }, last_y_ { 0 };
    bool have_last_ { false };
    std::vector<float> u_, v_, d_, u0_, v0_, d0_;
};

// ── a ray-marched signed-distance scene ──────────────────────────────

class Raymarch final : public Scene {
public:
    void resize(int w, int h) override {
        w_ = std::max(8, w); h_ = std::max(8, h);
        px_.assign(static_cast<std::size_t>(w_) * h_, 0xff0b0d12u);
        row_ = 0;
    }
    void step(double dt) override {
        t_ += dt;
        const int rows = std::max(1, static_cast<int>(h_ * std::min(1.0, dt * 3)));
        for (int k = 0; k < rows; ++k) { row_ = (row_ + 1) % h_; row_draw_(row_); }
    }
    void draw(std::span<std::uint32_t> px) const override { std::ranges::copy(px_ | std::views::take(px.size()), px.begin()); }
private:
    using V = std::array<double, 3>;
    static double len_(V v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }
    double sdf_(V p) const {
        const double bob = 0.25 * std::sin(t_ * 0.8);
        const double sphere = len_({p[0], p[1] - bob, p[2]}) - 1.0;
        const double plane = p[1] + 1.0;
        // A torus turning about the sphere.
        const double c = std::cos(t_ * 0.3), s = std::sin(t_ * 0.3);
        const V q{c * p[0] - s * p[2], p[1] - bob, s * p[0] + c * p[2]};
        const double torus = std::hypot(std::hypot(q[0], q[2]) - 1.8, q[1]) - 0.18;
        return std::min({sphere, plane, torus});
    }
    void row_draw_(int y) {
        const double a = t_ * 0.1;
        const V eye{4.5 * std::sin(a), 1.4, 4.5 * std::cos(a)};
        const V fwd{-eye[0] / len_(eye), -0.25, -eye[2] / len_(eye)};
        const V right{fwd[2], 0, -fwd[0]};
        const V up{0, 1, 0};
        for (int x = 0; x < w_; ++x) {
            const double u = (x - w_ / 2.0) / h_, v = -(y - h_ / 2.0) / h_;
            V d{fwd[0] + u * right[0] + v * up[0], fwd[1] + v, fwd[2] + u * right[2]};
            const double l = len_(d);
            for (auto& c : d) c /= l;
            double t = 0;
            bool hit = false;
            V p{};
            for (int i = 0; i < 64 && t < 30; ++i) {
                p = {eye[0] + d[0] * t, eye[1] + d[1] * t, eye[2] + d[2] * t};
                const double dist = sdf_(p);
                if (dist < 0.002) { hit = true; break; }
                t += dist;
            }
            std::uint32_t colour = argb_(0.05 + 0.05 * v, 0.06 + 0.05 * v, 0.09 + 0.08 * v);
            if (hit) {
                const double e = 0.001;
                V n{sdf_({p[0] + e, p[1], p[2]}) - sdf_({p[0] - e, p[1], p[2]}), sdf_({p[0], p[1] + e, p[2]}) - sdf_({p[0], p[1] - e, p[2]}),
                    sdf_({p[0], p[1], p[2] + e}) - sdf_({p[0], p[1], p[2] - e})};
                const double nl = len_(n);
                for (auto& c : n) c /= nl;
                const V light{0.5, 0.8, 0.3};
                const double diff = std::max(0.0, (n[0] * light[0] + n[1] * light[1] + n[2] * light[2]) / len_(light));
                const double fog = std::exp(-t * 0.08);
                const bool floor = p[1] < -0.99;
                const double check = floor ? ((static_cast<int>(std::floor(p[0])) + static_cast<int>(std::floor(p[2]))) & 1) * 0.06 : 0;
                colour = argb_((0.18 + 0.5 * diff + check) * fog, (0.2 + 0.45 * diff + check) * fog, (0.26 + 0.4 * diff + check) * fog);
            }
            px_[static_cast<std::size_t>(y) * w_ + x] = colour;
        }
    }
    int w_ { 0 }, h_ { 0 }, row_ { 0 };
    double t_ { 0 };
    std::vector<std::uint32_t> px_;
};

constexpr std::array<std::string_view, 11> kNames{"reaction-diffusion", "mandelbrot", "julia", "lorenz", "flow-field", "boids",
                                                  "life", "double-pendulum", "n-body", "fluid", "raymarch"};

}  // namespace

std::span<const std::string_view> names() { return kNames; }

std::unique_ptr<Scene> make(std::string_view name, std::uint32_t seed) {
    if (name == "mandelbrot") return std::make_unique<Fractal>(false);
    if (name == "julia") return std::make_unique<Fractal>(true);
    if (name == "lorenz") return std::make_unique<Lorenz>(seed);
    if (name == "flow-field") return std::make_unique<FlowField>(seed);
    if (name == "boids") return std::make_unique<Boids>(seed);
    if (name == "life") return std::make_unique<Life>(seed);
    if (name == "double-pendulum") return std::make_unique<Pendulums>(seed);
    if (name == "n-body") return std::make_unique<Bodies>(seed);
    if (name == "fluid") return std::make_unique<Fluid>(seed);
    if (name == "raymarch") return std::make_unique<Raymarch>();
    return std::make_unique<ReactionDiffusion>(seed);
}

}  // namespace xdesk::scenes
