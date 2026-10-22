module telemetry;

import std;

namespace telemetry {

namespace {

std::size_t bucket_(std::uint64_t v) { return v == 0 ? 0 : static_cast<std::size_t>(std::bit_width(v) - 1); }

void raise_max_(std::atomic<std::uint64_t>& m, std::uint64_t v) {
    auto cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

}  // namespace

void Gauge::raise_(std::int64_t v) {
    auto cur = high_.load(std::memory_order_relaxed);
    while (v > cur && !high_.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

void Gauge::set(std::int64_t v) {
    v_.store(v, std::memory_order_relaxed);
    raise_(v);
}

void Gauge::add(std::int64_t d) { raise_(v_.fetch_add(d, std::memory_order_relaxed) + d); }

void Histogram::record(std::uint64_t v) {
    b_[bucket_(v)].fetch_add(1, std::memory_order_relaxed);
    count_.fetch_add(1, std::memory_order_relaxed);
    sum_.fetch_add(v, std::memory_order_relaxed);
    raise_max_(max_, v);
}

std::uint64_t Histogram::quantile(double q) const {
    const auto b = buckets();
    std::uint64_t n = 0;
    for (const auto c : b) n += c;
    if (n == 0) return 0;
    const auto want = std::max<std::uint64_t>(1, static_cast<std::uint64_t>(std::ceil(std::clamp(q, 0.0, 1.0) * static_cast<double>(n))));
    std::uint64_t seen = 0;
    for (std::size_t i = 0; i < kBuckets; ++i) {
        seen += b[i];
        if (seen >= want) {
            const std::uint64_t upper = i >= 63 ? std::numeric_limits<std::uint64_t>::max() : (std::uint64_t{2} << i) - 1;
            return std::min(upper, max());
        }
    }
    return max();
}

std::array<std::uint64_t, Histogram::kBuckets> Histogram::buckets() const {
    std::array<std::uint64_t, kBuckets> out{};
    for (std::size_t i = 0; i < kBuckets; ++i) out[i] = b_[i].load(std::memory_order_relaxed);
    return out;
}

void Histogram::reset() {
    for (auto& x : b_) x.store(0, std::memory_order_relaxed);
    count_.store(0, std::memory_order_relaxed);
    sum_.store(0, std::memory_order_relaxed);
    max_.store(0, std::memory_order_relaxed);
}

Counter& Registry::counter(std::string_view name) {
    std::lock_guard l(m_);
    auto it = counters_.find(name);
    if (it == counters_.end()) it = counters_.emplace(std::string(name), std::make_unique<Counter>()).first;
    return *it->second;
}

Gauge& Registry::gauge(std::string_view name) {
    std::lock_guard l(m_);
    auto it = gauges_.find(name);
    if (it == gauges_.end()) it = gauges_.emplace(std::string(name), std::make_unique<Gauge>()).first;
    return *it->second;
}

Histogram& Registry::histogram(std::string_view name, std::string_view unit) {
    std::lock_guard l(m_);
    auto it = histograms_.find(name);
    if (it == histograms_.end())
        it = histograms_.emplace(std::string(name), std::pair{std::string(unit), std::make_unique<Histogram>()}).first;
    return *it->second.second;
}

Json Registry::snapshot() const {
    std::lock_guard l(m_);
    Json counters = Json::object(), gauges = Json::object(), histograms = Json::object();
    for (const auto& [name, c] : counters_) counters[name] = c->value();
    for (const auto& [name, g] : gauges_) gauges[name] = {{"value", g->value()}, {"high", g->high()}};
    for (const auto& [name, uh] : histograms_) {
        const auto& h = *uh.second;
        Json buckets = Json::array();
        const auto b = h.buckets();
        for (std::size_t i = 0; i < b.size(); ++i)
            if (b[i]) buckets.push_back({i, b[i]});
        histograms[name] = {{"unit", uh.first}, {"count", h.count()}, {"sum", h.sum()}, {"p50", h.quantile(0.5)},
                            {"p90", h.quantile(0.9)}, {"p99", h.quantile(0.99)}, {"max", h.max()}, {"buckets", std::move(buckets)}};
    }
    return {{"counters", std::move(counters)}, {"gauges", std::move(gauges)}, {"histograms", std::move(histograms)}};
}

void Registry::reset() {
    std::lock_guard l(m_);
    for (auto& [n, c] : counters_) c->reset();
    for (auto& [n, g] : gauges_) g->reset();
    for (auto& [n, h] : histograms_) h.second->reset();
}

Registry& metrics() {
    static Registry r;
    return r;
}

}  // namespace telemetry
