#pragma once

#include <cstdint>
#include <vector>

namespace hbfine {
// Physical time in picoseconds. The embedding simulator owns clock conversion.
using Time = std::uint64_t;
enum class Operation : std::uint8_t { Read, Write, Writeback };
enum class ReadPath : std::uint8_t { Host, Prefetch, Resident };
enum class Error : int { None = 0, UninitializedPage = 1, IncompletePageTimeout = 2, IllegalProgramOrder = 3 };
struct Request {
    std::uint64_t request_id, global_addr;
    int size_bytes;
    Operation operation;
    std::int64_t axi_id{-1}; // -1 assigns request_id modulo the 14-bit AXI ID space
    ReadPath read_path{ReadPath::Host};
};
struct Completion {
    std::uint64_t request_id, global_addr;
    Operation operation;
    Error error{Error::None};
};
struct Statistics {
    std::uint64_t accepted_requests{}, completed_requests{}, failed_requests{};
    std::uint64_t host_read_bytes{}, host_write_bytes{}, sensed_bytes{};
    std::uint64_t senses{}, programs{}, erases{}, cache_hits{}, cache_misses{}, cache_evictions{};
    std::uint64_t queue_stalls{}, bank_wait_ps{}, transfer_wait_ps{}, events_processed{};
    std::uint64_t elapsed_ps{};
    std::vector<std::uint64_t> stack_read_bytes, channel_read_bytes, bank_reads, plane_reads;
    std::vector<std::uint64_t> stack_busy_ps, channel_read_busy_ps, channel_write_busy_ps;
    std::vector<std::uint64_t> bank_busy_ps, plane_sense_busy_ps, plane_program_busy_ps, plane_erase_busy_ps;
    std::vector<std::uint64_t> die_transfer_busy_ps, ecc_busy_ps;
};

} // namespace hbfine
