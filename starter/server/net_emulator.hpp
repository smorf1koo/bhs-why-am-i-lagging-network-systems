#pragma once
// Built-in network emulator: every datagram gets its own random one-way delay (normal distribution,
// clipped at 0) and may be dropped. Works in user space, no root / tc netem needed.
//
// Datagrams are kept in a priority queue ordered by their due time. Because every datagram gets an
// independent delay, jitter larger than the gap between datagrams REORDERS them (like tc netem does).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <queue>
#include <random>
#include <vector>

#include "profiles.hpp"

namespace wail {

// Statistics of one direction of one client (for the summary printed at exit).
struct LinkStats {
    std::uint64_t passed = 0;
    std::uint64_t dropped = 0;
    std::vector<double> delays_ms;  // applied (sampled) delays of passed datagrams
};

class LinkSampler {
public:
    explicit LinkSampler(std::uint64_t seed) : rng_(seed) {}

    // Returns the delay in nanoseconds, or std::nullopt if the datagram is lost.
    std::optional<std::uint64_t> sample(const LinkParams& p, LinkStats& st) {
        if (p.loss_pct > 0 && uni_(rng_) * 100.0 < p.loss_pct) {
            ++st.dropped;
            return std::nullopt;
        }
        double d = p.delay_ms;
        if (p.jitter_ms > 0) d += p.jitter_ms * norm_(rng_);
        d = std::max(0.0, d);
        ++st.passed;
        st.delays_ms.push_back(d);
        return static_cast<std::uint64_t>(std::llround(d * 1e6));
    }

private:
    std::mt19937_64 rng_;
    std::uniform_real_distribution<double> uni_{0.0, 1.0};
    std::normal_distribution<double> norm_{0.0, 1.0};
};

// Min-heap of items by due time; ties are broken by insertion order (FIFO).
template <class T>
class DelayQueue {
public:
    void push(std::uint64_t due_ns, T item) { heap_.push(Entry{due_ns, next_order_++, std::move(item)}); }
    bool empty() const { return heap_.empty(); }
    std::size_t size() const { return heap_.size(); }
    std::uint64_t next_due() const { return heap_.top().due_ns; }
    // Pops the earliest item if it is due at `now_ns`. Returns its due time through `due_out`.
    std::optional<T> pop_due(std::uint64_t now_ns, std::uint64_t& due_out) {
        if (heap_.empty() || heap_.top().due_ns > now_ns) return std::nullopt;
        Entry e = std::move(const_cast<Entry&>(heap_.top()));
        heap_.pop();
        due_out = e.due_ns;
        return std::move(e.item);
    }

private:
    struct Entry {
        std::uint64_t due_ns;
        std::uint64_t order;
        T item;
        bool operator>(const Entry& o) const { return due_ns != o.due_ns ? due_ns > o.due_ns : order > o.order; }
    };
    std::priority_queue<Entry, std::vector<Entry>, std::greater<Entry>> heap_;
    std::uint64_t next_order_ = 0;
};

}  // namespace wail
