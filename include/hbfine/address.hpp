#pragma once

#include "config.hpp"
#include <stdexcept>

namespace hbfine {

struct Address {
    int stack{}, channel{}, virtual_port{}, bank{}, die{}, plane{}, block{}, page{}, byte{};
};

// Whole-page striping keeps a Core Die page within one physical Host Channel.
// AXI ports are virtual and may stripe its 64B beats without adding bandwidth.
inline Address decode_address(std::uint64_t address, std::uint64_t base,
                                     const Config& c) {
    const auto capacity = static_cast<std::uint64_t>(c.stacks) * capacity_bytes_per_stack(c);
    if (address < base || address - base >= capacity) throw std::out_of_range("HBF physical address outside stack capacity");
    const auto offset = address - base;
    const auto channels = static_cast<std::uint64_t>(c.stacks * c.host_channels_per_stack);
    const auto stripe = static_cast<std::uint64_t>(c.host_stripe_bytes);
    const auto channel = (offset / stripe) % channels;
    const auto local = (offset / (stripe * channels)) * stripe + offset % stripe;
    auto page = local / 4096;
    Address result;
    result.stack = static_cast<int>(channel / c.host_channels_per_stack);
    result.channel = static_cast<int>(channel % c.host_channels_per_stack);
    result.virtual_port = static_cast<int>((local / 64) % c.virtual_axi_ports_per_channel);
    result.byte = static_cast<int>(local % 4096);
    result.bank = static_cast<int>(page % c.banks_per_channel);
    page /= c.banks_per_channel;
    const int plane_in_bank = static_cast<int>(page % c.planes_per_bank);
    page /= c.planes_per_bank;
    const int physical_plane = (result.channel * c.banks_per_channel + result.bank) * c.planes_per_bank + plane_in_bank;
    result.die = physical_plane / c.planes_per_die;
    result.plane = physical_plane % c.planes_per_die;
    result.page = static_cast<int>(page % c.pages_per_block);
    result.block = static_cast<int>(page / c.pages_per_block);
    return result;
}

inline std::uint64_t encode_address(const Address& a, std::uint64_t base,
                                       const Config& c) {
    const int physical_plane = a.die * c.planes_per_die + a.plane;
    if (a.stack < 0 || a.stack >= c.stacks || a.channel < 0 || a.channel >= c.host_channels_per_stack ||
        a.bank < 0 || a.bank >= c.banks_per_channel || a.die < 0 || a.die >= c.dies_per_stack ||
        a.plane < 0 || a.plane >= c.planes_per_die || a.block < 0 || a.block >= c.blocks_per_plane ||
        a.page < 0 || a.page >= c.pages_per_block || a.byte < 0 || a.byte >= 4096 ||
        physical_plane / c.planes_per_bank != a.channel * c.banks_per_channel + a.bank) {
        throw std::invalid_argument("contradictory HBF Bank/Plane address");
    }
    const auto local_page = ((static_cast<std::uint64_t>(a.block) * c.pages_per_block + a.page) *
                             c.planes_per_bank + physical_plane % c.planes_per_bank) * c.banks_per_channel + a.bank;
    const auto local = local_page * 4096 + a.byte;
    const auto channels = static_cast<std::uint64_t>(c.stacks * c.host_channels_per_stack);
    return base + ((local / c.host_stripe_bytes) * channels + a.stack * c.host_channels_per_stack + a.channel) *
                    c.host_stripe_bytes + local % c.host_stripe_bytes;
}

inline double array_read_ceiling_GBps(const Config& c) {
    return c.host_channels_per_stack * c.banks_per_channel * 4096.0 / (c.sense_latency_us * 1000.0);
}
inline double array_write_ceiling_GBps(const Config& c) {
    return c.host_channels_per_stack * c.banks_per_channel * 4096.0 / (c.program_latency_us * 1000.0);
}

} // namespace hbfine
