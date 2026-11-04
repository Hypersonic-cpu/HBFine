#include <hbfine/controller.hpp>
#include <hbfine/address.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <functional>
#include <map>
#include <limits>
#include <queue>
#include <tuple>
#include <unordered_map>
#include <unordered_set>

namespace hbfine {
namespace {
constexpr std::uint64_t no_page = std::numeric_limits<std::uint64_t>::max();
}
struct Controller::Impl {
public:
    Impl(const Config& config, std::uint64_t base, std::size_t capacity)
        : c_(config), base_(base), capacity_(capacity),
          channels_(c_.stacks * c_.host_channels_per_stack),
          banks_(scheduling_unit_count(c_)),
          dispatch_next_(c_.stacks), die_next_(c_.stacks * c_.dies_per_stack), ecc_next_(c_.stacks), sram_next_(c_.enable_read_sram ? c_.stacks : 0) {
        const auto planes = static_cast<std::size_t>(c_.stacks * c_.dies_per_stack * c_.planes_per_die);
        stats_.stack_read_bytes.resize(c_.stacks);
        stats_.channel_read_bytes.resize(channels_.size());
        stats_.bank_reads.resize(banks_.size());
        stats_.plane_reads.resize(planes);
        stats_.stack_busy_ps.resize(c_.stacks);
        stats_.channel_read_busy_ps.resize(channels_.size());
        stats_.channel_write_busy_ps.resize(channels_.size());
        stats_.bank_busy_ps.resize(banks_.size());
        stats_.plane_sense_busy_ps.resize(planes);
        stats_.plane_program_busy_ps.resize(planes);
        stats_.plane_erase_busy_ps.resize(planes);
        stats_.die_transfer_busy_ps.resize(die_next_.size());
        stats_.ecc_busy_ps.resize(c_.stacks);
    }

    void preload_region(std::uint64_t address, std::uint64_t bytes) {
        if (stats_.accepted_requests != 0 || last_time_ != 0 || address % 4096 || bytes % 4096 || bytes == 0)
            throw std::invalid_argument("preload must be a nonempty page-aligned initialization region");
        decode_address(address, base_, c_);
        if (bytes > static_cast<std::uint64_t>(c_.stacks) * capacity_bytes_per_stack(c_) - (address - base_))
            throw std::out_of_range("preload exceeds HBF capacity");
        preloaded_.push_back({address, address + bytes});
    }

    bool try_submit(const Request& request, Time now) {
        if (now < last_time_ || request.size_bytes <= 0 || request.size_bytes > 4096 ||
            request.size_bytes % 64 || request.global_addr % 64 ||
            request.global_addr % 4096 + request.size_bytes > 4096 ||
            request.axi_id > 16383 || request.axi_id < -1)
            throw std::invalid_argument("HBF requires ordered, aligned 64B-multiple requests within one 4KiB page");
        const bool write = request.operation != hbfine::Operation::Read;
        if (write && request.operation != hbfine::Operation::Write && request.operation != hbfine::Operation::Writeback)
            throw std::invalid_argument("unsupported HBF operation");
        if (write && !c_.allow_program) throw std::invalid_argument("HBF programs disabled in read-only profile");
        if (request.read_path != ReadPath::Host && (!c_.enable_read_sram || write))
            throw std::invalid_argument("SRAM path requires an enabled read-only device");
        const auto address = decode_address(request.global_addr, base_, c_);
        const auto channel = channel_id(address);
        const auto page = request.global_addr / 4096 * 4096;
        const int beats = request.size_bytes / 64;
        if (parents_.contains(request.request_id)) throw std::logic_error("duplicate HBF request ID");
        if (parents_.size() >= capacity_ || channels_[channel].queued_beats + beats > c_.channel_queue_capacity ||
            (write && !writes_.contains(page) && channels_[channel].write_pages >= c_.write_pages_per_channel)) {
            ++stats_.queue_stalls;
            return false;
        }
        const auto mask = beats == 64 ? ~0ULL : ((1ULL << beats) - 1) << (address.byte / 64);
        if (write && writes_.contains(page) && (writes_.at(page).reserved_mask & mask))
            throw std::invalid_argument("overlapping HBF write pieces require a host-managed new block");
        if (write && writes_.contains(page) && writes_.at(page).error != Error::None)
            throw std::invalid_argument("cannot add pieces to a failed HBF page");

        last_time_ = now;
        const auto axi = request.axi_id < 0 ? request.request_id % 16384 :
                                                    static_cast<std::uint64_t>(request.axi_id);
        const OrderKey order{channel, axi, write, request.read_path == ReadPath::Prefetch};
        parents_.emplace(request.request_id, Parent{request, beats, order});
        ordering_[order].push_back(request.request_id);
        channels_[channel].queued_beats += beats;
        ++stats_.accepted_requests;
        if (write) {
            if (!writes_.contains(page)) {
                writes_.emplace(page, Accumulator{});
                writes_.at(page).generation = ++write_generation_;
                ++channels_[channel].write_pages;
            }
            writes_.at(page).reserved_mask |= mask;
            stats_.host_write_bytes += request.size_bytes;
        } else if (request.read_path != ReadPath::Prefetch) {
            stats_.host_read_bytes += request.size_bytes;
        }
        const Time arrival = now;
        for (int i = 0; i < beats; ++i) {
            Beat beat{request.request_id, request.global_addr + static_cast<std::uint64_t>(64 * i), arrival};
            if (write) {
                auto& ch = channels_[channel];
                const Time start = std::max(arrival, ch.write_next);
                const Time duration = transfer_time(64, c_.write_target_GBps_per_stack / 16);
                ch.write_next = start + duration;
                stats_.channel_write_busy_ps[channel] += duration;
                stats_.transfer_wait_ps += start - arrival;
                schedule(ch.write_next, [this, beat] { receive_write(beat); });
            } else {
                const Time start = std::max(arrival, dispatch_next_[address.stack]);
                dispatch_next_[address.stack] = start + ns(c_.dispatch_interval_ns);
                const Time ready = start + ns(c_.dispatch_latency_ns);
                schedule(ready, [this, beat] { receive_read(beat); });
            }
        }
        return true;
    }

    std::vector<Completion> advance(Time now) {
        if (now < last_time_) throw std::invalid_argument("HBF time moved backwards");
        const Time limit = now;
        while (!events_.empty() && events_.top().time <= limit) {
            auto event = events_.top();
            events_.pop();
            current_ = event.time;
            ++stats_.events_processed;
            event.action();
        }
        current_ = limit;
        stats_.elapsed_ps = limit;
        last_time_ = now;
        std::vector<Completion> result;
        result.swap(completed_);
        return result;
    }

    std::size_t pending_requests() const { return parents_.size() + completed_.size(); }
    const Statistics& statistics() const { return stats_; }

private:
    using OrderKey = std::tuple<int, std::uint64_t, bool, bool>;
    struct Parent { Request request; int remaining; OrderKey order; bool ready{}; Error error{Error::None}; };
    struct Beat { std::uint64_t parent, address; Time arrival; };
    struct Buffer { std::uint64_t page{no_page}; bool ready{}; int pins{}; Time last_use{}; std::vector<Beat> waiting; };
    struct Operation { bool program; Beat beat; };
    struct Bank { bool busy{}; bool wake_pending{}; std::array<Buffer, 2> buffers; std::deque<Operation> queue; };
    struct Channel { int queued_beats{}; int write_pages{}; Time read_next{}, write_next{}; };
    struct Accumulator {
        std::uint64_t reserved_mask{}, arrived_mask{};
        bool timer_started{}, full{};
        Error error{Error::None};
        std::uint64_t generation{};
        std::vector<Beat> pieces;
    };
    struct Region { std::uint64_t begin, end;  };
    struct Event {
        Time time, serial;
        std::function<void()> action;
        bool operator>(const Event& other) const { return time != other.time ? time > other.time : serial > other.serial; }
    };

    Config c_;
    std::uint64_t base_;
    std::size_t capacity_;
    Time current_{}, serial_{};
    std::uint64_t write_generation_{};
    Time last_time_{};
    std::vector<Channel> channels_;
    std::vector<Bank> banks_;
    std::vector<Time> dispatch_next_, die_next_, ecc_next_, sram_next_;
    std::priority_queue<Event, std::vector<Event>, std::greater<Event>> events_;
    std::unordered_map<std::uint64_t, Parent> parents_;
    std::map<OrderKey, std::deque<std::uint64_t>> ordering_;
    std::unordered_map<std::uint64_t, Accumulator> writes_;
    std::unordered_map<std::uint64_t, int> next_program_page_;
    std::unordered_set<std::uint64_t> programmed_pages_;
    std::vector<Region> preloaded_;
    std::vector<Completion> completed_;
    Statistics stats_;

    static Time us(double value) { return static_cast<Time>(std::ceil(value * 1000000)); }
    static Time ns(double value) { return static_cast<Time>(std::ceil(value * 1000)); }
    static Time transfer_time(int bytes, double bandwidth) { return static_cast<Time>(std::ceil(bytes * 1000.0 / bandwidth)); }
    int channel_id(const Address& a) const { return a.stack * 16 + a.channel; }
    void schedule(Time time, std::function<void()> action) { events_.push({time, serial_++, std::move(action)}); }

    bool valid_page(std::uint64_t page) const {
        if (programmed_pages_.contains(page)) return true;
        return std::any_of(preloaded_.begin(), preloaded_.end(), [page](const auto& region) {
            return page >= region.begin && page < region.end;
        });
    }

    void finish(const Beat& beat, Error error = Error::None) {
        auto& parent = parents_.at(beat.parent);
        if (parent.remaining <= 0) throw std::logic_error("duplicate HBF beat completion");
        if (error != Error::None) parent.error = error;
        const auto a = decode_address(beat.address, base_, c_);
        --channels_[channel_id(a)].queued_beats;
        if (--parent.remaining != 0) return;
        parent.ready = true;
        const auto key = parent.order;
        auto& queue = ordering_.at(key);
        while (!queue.empty() && parents_.at(queue.front()).ready) {
            const auto id = queue.front();
            const auto done = parents_.at(id);
            const auto& r = done.request;
            completed_.push_back({r.request_id, r.global_addr, r.operation, done.error});
            ++stats_.completed_requests;
            if (done.error != Error::None) ++stats_.failed_requests;
            parents_.erase(id);
            queue.pop_front();
        }
        if (queue.empty()) ordering_.erase(key);
    }

    Time sram_transfer(int stack) {
        const Time start = std::max(current_, sram_next_[stack]);
        sram_next_[stack] = start + transfer_time(64, c_.read_sram_bandwidth_GBps_per_stack);
        stats_.transfer_wait_ps += start - current_;
        return sram_next_[stack] + ns(c_.read_sram_access_latency_ns);
    }

    void transfer_host(const Beat& beat, const Address& address, int bank, int slot) {
        const int channel = channel_id(address);
        auto& ch = channels_[channel];
        const Time start = std::max(current_, ch.read_next);
        const Time duration = transfer_time(64, c_.read_target_GBps_per_stack / 16);
        ch.read_next = start + duration;
        stats_.transfer_wait_ps += start - current_;
        stats_.channel_read_busy_ps[channel] += duration;
        stats_.channel_read_bytes[channel] += 64;
        stats_.stack_read_bytes[address.stack] += 64;
        ++stats_.bank_reads[bank];
        ++stats_.plane_reads[physical_plane_id(address, c_)];
        schedule(ch.read_next, [this, beat, bank, slot] {
            if (slot >= 0) --banks_[bank].buffers[slot].pins;
            finish(beat);
            if (slot >= 0) wake(bank);
        });
    }

    void transfer_read(const Beat& beat, int bank, int slot) {
        const auto address = decode_address(beat.address, base_, c_);
        auto& buffer = banks_[bank].buffers[slot];
        ++buffer.pins;
        buffer.last_use = current_;
        if (parents_.at(beat.parent).request.read_path == ReadPath::Prefetch) {
            schedule(sram_transfer(address.stack), [this, beat, bank, slot] {
                --banks_[bank].buffers[slot].pins;
                finish(beat);
                wake(bank);
            });
        } else {
            transfer_host(beat, address, bank, slot);
        }
    }

    void receive_read(const Beat& beat) {
        const auto page = beat.address / 4096 * 4096;
        if (!valid_page(page)) { finish(beat, Error::UninitializedPage); return; }
        const auto address = decode_address(beat.address, base_, c_);
        const auto mapping = map_read(address, beat.address, base_, c_);
        const int bank = mapping.unit;
        if (parents_.at(beat.parent).request.read_path == ReadPath::Resident) {
            schedule(sram_transfer(address.stack), [this, beat, address, bank] {
                transfer_host(beat, address, bank, -1);
            });
            return;
        }
        for (int slot = 0; slot < 2; ++slot) {
            auto& buffer = banks_[bank].buffers[slot];
            if (buffer.page != mapping.buffer_key) continue;
            if (buffer.ready) { ++stats_.cache_hits; transfer_read(beat, bank, slot); }
            else { ++stats_.cache_misses; buffer.waiting.push_back(beat); }
            return;
        }
        ++stats_.cache_misses;
        banks_[bank].queue.push_back({false, beat});
        wake(bank);
    }

    Time core_transfer(Time ready, const Address& address, int bytes = 4096) {
        const auto reserve = [&](Time& next, double rate, std::uint64_t& busy) {
            if (rate == 0) return;
            const Time start = std::max(ready, next);
            const Time duration = transfer_time(bytes, rate);
            stats_.transfer_wait_ps += start - ready;
            busy += duration;
            ready = next = start + duration;
        };
        const int die = address.stack * c_.dies_per_stack + address.die;
        reserve(die_next_[die], c_.core_transfer_GBps_per_die, stats_.die_transfer_busy_ps[die]);
        reserve(ecc_next_[address.stack], c_.ecc_GBps_per_stack, stats_.ecc_busy_ps[address.stack]);
        return ready;
    }

    void wake(int bank) {
        auto& b = banks_[bank];
        if (b.wake_pending || b.queue.empty()) return;
        b.wake_pending = true;
        schedule(current_, [this, bank] { banks_[bank].wake_pending = false; dispatch_bank(bank); });
    }

    void dispatch_bank(int bank) {
        auto& b = banks_[bank];
        while (!b.queue.empty()) {
            const auto operation = b.queue.front();
            const auto address = decode_address(operation.beat.address, base_, c_);
            const auto mapping = map_read(address, operation.beat.address, base_, c_);
            const auto page = mapping.buffer_key;
            if (!operation.program) {
                int matching = -1;
                for (int slot = 0; slot < 2; ++slot) if (b.buffers[slot].page == page) matching = slot;
                if (matching >= 0) {
                    auto& buffer = b.buffers[matching];
                    b.queue.pop_front();
                    if (buffer.ready) transfer_read(operation.beat, bank, matching);
                    else buffer.waiting.push_back(operation.beat);
                    continue;
                }
                if (b.busy) return;
                int slot = -1;
                for (int i = 0; i < 2; ++i) {
                    const auto& candidate = b.buffers[i];
                    if (candidate.pins || (!candidate.ready && candidate.page != no_page)) continue;
                    if (slot < 0 || candidate.page == no_page || candidate.last_use < b.buffers[slot].last_use) slot = i;
                }
                if (slot < 0) return; // Two pinned/filling buffers do not add sense engines.
                auto& buffer = b.buffers[slot];
                if (buffer.page != no_page) ++stats_.cache_evictions;
                buffer = Buffer{};
                buffer.page = page;
                buffer.waiting.push_back(operation.beat);
                b.queue.pop_front();
                b.busy = true;
                const Time duration = us(c_.sense_latency_us);
                const int sensed_planes = mapping.plane_count;
                stats_.senses += sensed_planes;
                stats_.sensed_bytes += sensed_planes * c_.page_bytes;
                stats_.bank_wait_ps += current_ - operation.beat.arrival;
                stats_.bank_busy_ps[bank] += duration;
                for (int lane = 0; lane < sensed_planes; ++lane)
                    stats_.plane_sense_busy_ps[mapping.first_plane + lane] += duration;
                stats_.stack_busy_ps[address.stack] += duration;
                schedule(current_ + duration, [this, bank, slot, address, sensed_planes] {
                    banks_[bank].busy = false;
                    const Time ready = core_transfer(current_, address, sensed_planes * c_.page_bytes);
                    schedule(ready, [this, bank, slot] {
                        auto& buffer = banks_[bank].buffers[slot];
                        buffer.ready = true;
                        auto waiters = std::move(buffer.waiting);
                        buffer.waiting.clear();
                        for (const auto& beat : waiters) transfer_read(beat, bank, slot);
                        wake(bank);
                    });
                    wake(bank);
                });
            } else {
                if (b.busy) return;
                b.queue.pop_front();
                b.busy = true;
                stats_.bank_wait_ps += current_ - operation.beat.arrival;
                const Time ready = core_transfer(current_, address);
                schedule(ready, [this, bank, page, address] {
                    if (address.page == 0) {
                        ++stats_.erases;
                        const Time duration = us(c_.erase_latency_us);
                        stats_.plane_erase_busy_ps[physical_plane_id(address, c_)] += duration;
                        stats_.bank_busy_ps[bank] += duration;
                        stats_.stack_busy_ps[address.stack] += duration;
                        schedule(current_ + duration, [this, bank, page, address] { program(bank, page, address); });
                    } else program(bank, page, address);
                });
            }
        }
    }

    void receive_write(const Beat& beat) {
        const auto page = beat.address / 4096 * 4096;
        auto& accumulation = writes_.at(page);
        accumulation.arrived_mask |= 1ULL << ((beat.address % 4096) / 64);
        accumulation.pieces.push_back(beat);
        if (accumulation.error != Error::None) {
            finish(beat, accumulation.error);
            if (accumulation.arrived_mask == accumulation.reserved_mask) release_write(page);
            return;
        }
        if (!accumulation.timer_started) {
            accumulation.timer_started = true;
            schedule(current_ + us(c_.write_timeout_us), [this, page, generation = accumulation.generation] {
                if (writes_.contains(page) && writes_.at(page).generation == generation &&
                    !writes_.at(page).full && writes_.at(page).error == Error::None) fail_write(page, Error::IncompletePageTimeout);
            });
        }
        if (accumulation.arrived_mask != ~0ULL) return;
        accumulation.full = true;
        const auto address = decode_address(page, base_, c_);
        auto block_address = address;
        block_address.page = 0;
        const auto block_key = encode_address(block_address, base_, c_);
        // Fresh blocks only. The host must choose another block for any update;
        // this device never hides an FTL, valid-page migration or garbage collector.
        bool protected_block = false;
        for (int p = 0; p < c_.pages_per_block && !protected_block; ++p) {
            block_address.page = p;
            const auto physical_page = encode_address(block_address, base_, c_);
            protected_block = std::any_of(preloaded_.begin(), preloaded_.end(), [physical_page](const auto& r) {
                return physical_page >= r.begin && physical_page < r.end;
            });
        }
        if (protected_block || address.page != next_program_page_[block_key]) { fail_write(page, Error::IllegalProgramOrder); return; }
        ++next_program_page_[block_key];
        const int bank = scheduling_unit_id(address, c_);
        banks_[bank].queue.push_back({true, Beat{0, page, current_}});
        wake(bank);
    }

    void fail_write(std::uint64_t page, Error error) {
        auto& accumulation = writes_.at(page);
        accumulation.error = error;
        // Timeout cannot strand pieces already admitted but still in the input
        // serializer. Retain a tombstone and fail each of those on arrival.
        for (const auto& beat : accumulation.pieces) finish(beat, error);
        accumulation.pieces.clear();
        if (accumulation.arrived_mask == accumulation.reserved_mask) release_write(page);
    }

    void release_write(std::uint64_t page) {
        --channels_[channel_id(decode_address(page, base_, c_))].write_pages;
        writes_.erase(page);
    }

    void program(int bank, std::uint64_t page, Address address) {
        ++stats_.programs;
        const Time duration = us(c_.program_latency_us);
        stats_.plane_program_busy_ps[physical_plane_id(address, c_)] += duration;
        stats_.bank_busy_ps[bank] += duration;
        stats_.stack_busy_ps[address.stack] += duration;
        schedule(current_ + duration, [this, bank, page, address] {
            programmed_pages_.insert(page);
            auto accumulation = std::move(writes_.at(page));
            writes_.erase(page);
            --channels_[channel_id(address)].write_pages;
            for (const auto& beat : accumulation.pieces) finish(beat);
            banks_[bank].busy = false;
            wake(bank);
        });
    }
};

Controller::Controller(const Config& config, std::uint64_t base, std::size_t capacity) {
    validate_config(config);
    if (!capacity || base % 4096 || base > std::numeric_limits<std::uint64_t>::max() -
        static_cast<std::uint64_t>(config.stacks) * capacity_bytes_per_stack(config))
        throw std::invalid_argument("invalid HBF device base or request capacity");
    impl_ = std::make_unique<Impl>(config, base, capacity);
}
Controller::~Controller() = default;
bool Controller::try_submit(const Request& request, Time now) { return impl_->try_submit(request, now); }
std::vector<Completion> Controller::advance(Time now) { return impl_->advance(now); }
std::size_t Controller::pending_requests() const { return impl_->pending_requests(); }
const Statistics& Controller::statistics() const { return impl_->statistics(); }
void Controller::preload_region(std::uint64_t address, std::uint64_t bytes) {
    impl_->preload_region(address, bytes);
}
} // namespace hbfine
