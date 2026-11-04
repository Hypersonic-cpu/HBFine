#pragma once

#include <cstdint>

namespace hbfine {

enum class GeometryProfile { OCPReference, HBFlexResearch, GroupedPlanes };

struct Config {
    GeometryProfile geometry{GeometryProfile::HBFlexResearch};
    int stacks{1};
    int host_channels_per_stack{16};
    int virtual_axi_ports_per_channel{1};
    int dies_per_stack{16};
    int planes_per_die{32};
    int banks_per_channel{32};
    int planes_per_bank{1};
    int planes_per_command_group{1};
    int blocks_per_plane{512};
    int pages_per_block{512};
    int page_bytes{4096};
    int page_buffers_per_bank{2};
    int host_stripe_bytes{4096};
    int channel_queue_capacity{4096};
    int write_pages_per_channel{64};
    double read_target_GBps_per_stack{488.0};
    double write_target_GBps_per_stack{27.2};
    double ucie_ceiling_GBps_per_stack{1536.0};
    double sense_latency_us{4.0};
    double program_latency_us{75.0};
    // Neither source specifies an HBF erase time or a host timeout. Writes
    // require explicit values; read-only profiles never invent either one.
    bool allow_program{false};
    double erase_latency_us{0.0};
    double write_timeout_us{0.0};
    double dispatch_latency_ns{0.0};
    double dispatch_interval_ns{0.0};
    double core_transfer_GBps_per_die{0.0};
    double ecc_GBps_per_stack{0.0};
    std::uint64_t sram_bytes_per_plane{};
    std::uint64_t base_die_sram_bytes_per_stack{40000000};
};

inline std::uint64_t capacity_bytes_per_stack(const Config& c) {
    return static_cast<std::uint64_t>(c.dies_per_stack) * c.planes_per_die *
           c.blocks_per_plane * c.pages_per_block * c.page_bytes;
}

// Throws std::invalid_argument before constructing any device resources.
void validate_config(const Config& config);

} // namespace hbfine
