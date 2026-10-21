#include <hbfine/config.hpp>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace hbfine {
void validate_config(const Config& c) {
    const auto valid_bandwidth = [](double value) { return std::isfinite(value) && value > 0; };
    const int expected_banks = c.geometry == GeometryProfile::OCPReference ? 16 : 32;
    const auto nonnegative = [](double value) { return std::isfinite(value) && value >= 0; };
    if ((c.geometry != GeometryProfile::OCPReference && c.geometry != GeometryProfile::HBFlexResearch) || c.stacks < 1 || c.stacks > std::numeric_limits<int>::max() / 512 || c.host_channels_per_stack != 16 ||
        (c.virtual_axi_ports_per_channel != 1 && c.virtual_axi_ports_per_channel != 2 && c.virtual_axi_ports_per_channel != 4) ||
        c.dies_per_stack != 16 || c.planes_per_die != 32 || c.banks_per_channel != expected_banks ||
        c.planes_per_bank != (expected_banks == 16 ? 2 : 1) ||
        c.planes_per_bank * c.banks_per_channel * c.host_channels_per_stack != c.dies_per_stack * c.planes_per_die ||
        c.blocks_per_plane != 512 || c.pages_per_block != 512 || c.page_bytes != 4096 ||
        c.page_buffers_per_bank != 2 || c.host_stripe_bytes < 4096 || c.host_stripe_bytes % 4096 ||
        (512ULL << 30) % (static_cast<std::uint64_t>(c.host_stripe_bytes) * 16) ||
        c.channel_queue_capacity < 64 || c.write_pages_per_channel <= 0 ||
        !valid_bandwidth(c.read_target_GBps_per_stack) || !valid_bandwidth(c.write_target_GBps_per_stack) ||
        !valid_bandwidth(c.ucie_ceiling_GBps_per_stack) ||
        c.read_target_GBps_per_stack > c.ucie_ceiling_GBps_per_stack ||
        c.write_target_GBps_per_stack > c.ucie_ceiling_GBps_per_stack ||
        !valid_bandwidth(c.sense_latency_us) || !valid_bandwidth(c.program_latency_us) ||
        !nonnegative(c.erase_latency_us) || !nonnegative(c.write_timeout_us) ||
        !nonnegative(c.dispatch_latency_ns) || !nonnegative(c.dispatch_interval_ns) ||
        !nonnegative(c.core_transfer_GBps_per_die) || !nonnegative(c.ecc_GBps_per_stack) ||
        (c.allow_program && (c.erase_latency_us <= 0 || c.write_timeout_us <= 0)) ||
        static_cast<std::uint64_t>(c.write_pages_per_channel) * 4096 * 16 > c.base_die_sram_bytes_per_stack) {
        throw std::invalid_argument("inconsistent HBF Bank/Plane geometry, rates, buffers or write timing policy");
    }
}
} // namespace hbfine
