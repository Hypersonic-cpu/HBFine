#include <hbfine/config.hpp>
#include <cmath>
#include <stdexcept>
#include <limits>

namespace hbfine {
void validate_config(const Config& c) {
    const auto valid_bandwidth = [](double value) { return std::isfinite(value) && value > 0; };
    const int planes_per_bank = c.geometry == GeometryProfile::OCPReference ? 2 : 1;
    const int expected_banks = (c.geometry == GeometryProfile::OCPReference ? 16 : 32) /
                               (c.dies_per_stack == 8 ? 2 : 1);
    const auto nonnegative = [](double value) { return std::isfinite(value) && value >= 0; };
    const bool grouped = c.geometry == GeometryProfile::GroupedPlanes;
    const bool legacy_geometry =
        (c.geometry == GeometryProfile::OCPReference || c.geometry == GeometryProfile::HBFlexResearch) &&
        (c.dies_per_stack == 8 || c.dies_per_stack == 16) && c.planes_per_die == 32 &&
        c.banks_per_channel == expected_banks && c.planes_per_bank == planes_per_bank &&
        c.planes_per_command_group == 1 && c.blocks_per_plane == 512 && c.pages_per_block == 512;
    const bool grouped_geometry = grouped && !c.allow_program &&
        c.dies_per_stack > 0 && c.dies_per_stack <= 64 &&
        c.planes_per_die > 0 && c.planes_per_die <= 256 &&
        c.planes_per_command_group > 1 && c.planes_per_die % c.planes_per_command_group == 0 &&
        c.planes_per_bank == 1 && c.banks_per_channel > 0 && c.banks_per_channel <= 1024 &&
        c.banks_per_channel * 16 == c.dies_per_stack * c.planes_per_die &&
        c.blocks_per_plane > 0 && c.blocks_per_plane <= 65536 &&
        c.pages_per_block > 0 && c.pages_per_block <= 65536 &&
        c.host_stripe_bytes == 4096;
    if (c.enable_read_sram &&
        (c.allow_program || (!c.sram_bytes_per_plane && !c.base_die_sram_bytes_per_stack) ||
         !valid_bandwidth(c.read_sram_access_latency_ns) ||
         !valid_bandwidth(c.read_sram_bandwidth_GBps_per_stack)))
        throw std::invalid_argument("invalid read SRAM configuration");
    if ((!legacy_geometry && !grouped_geometry) || c.stacks < 1 ||
        c.stacks > std::numeric_limits<int>::max() / (c.dies_per_stack * c.planes_per_die) ||
        c.host_channels_per_stack != 16 ||
        (c.virtual_axi_ports_per_channel != 1 && c.virtual_axi_ports_per_channel != 2 &&
         c.virtual_axi_ports_per_channel != 4) || c.page_bytes != 4096 ||
        c.page_buffers_per_bank != 2 || c.host_stripe_bytes < 4096 || c.host_stripe_bytes % 4096 ||
        (!grouped && capacity_bytes_per_stack(c) % (static_cast<std::uint64_t>(c.host_stripe_bytes) * 16)) ||
        capacity_bytes_per_stack(c) > std::numeric_limits<std::uint64_t>::max() / c.stacks ||
        c.channel_queue_capacity < 64 || c.write_pages_per_channel <= 0 ||
        !valid_bandwidth(c.read_target_GBps_per_stack) || !valid_bandwidth(c.write_target_GBps_per_stack) ||
        !valid_bandwidth(c.ucie_ceiling_GBps_per_stack) ||
        (grouped && c.read_target_GBps_per_stack >
             c.dies_per_stack * c.planes_per_die * 4096.0 / (c.sense_latency_us * 1000.0)) ||
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
