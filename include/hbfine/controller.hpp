#pragma once

#include "config.hpp"
#include "types.hpp"
#include <memory>

namespace hbfine {
// Timing-only device: owns no payload, host allocator, FTL, GC, or GPU state.
// Rejection retains caller ownership; acceptance yields exactly one completion.
// Not thread-safe. Time must be monotonic; completion callbacks are never reentrant.
class Controller {
public:
    explicit Controller(const Config& config, std::uint64_t base_address = 0,
                        std::size_t request_capacity = 65536);
    ~Controller();
    Controller(const Controller&) = delete;
    Controller& operator=(const Controller&) = delete;
    bool try_submit(const Request& request, Time now_ps);
    std::vector<Completion> advance(Time now_ps);
    std::size_t pending_requests() const;
    const Statistics& statistics() const;
    // Initialization only: declares valid pages without warming caches or timed writes.
    void preload_region(std::uint64_t address, std::uint64_t bytes);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace hbfine
