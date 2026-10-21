#include <hbfine/address.hpp>
#include <hbfine/controller.hpp>
#include <iostream>
#include <random>
#include <stdexcept>

using namespace hbfine;
namespace {
void require(bool value, const char* reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void rejects(F action) {
    bool rejected = false;
    try { action(); } catch (const std::exception&) { rejected = true; }
    require(rejected, "invalid configuration/request accepted");
}
std::uint64_t page(const Config& c, int bank, int index = 0, int plane = 0) {
    Address a;
    a.bank = bank; a.page = index;
    const int physical = bank * c.planes_per_bank + plane;
    a.die = physical / c.planes_per_die; a.plane = physical % c.planes_per_die;
    return encode_address(a, 0, c);
}
void mapping_and_read_service() {
    std::mt19937_64 random(1021);
    for (auto geometry : {GeometryProfile::OCPReference, GeometryProfile::HBFlexResearch}) {
        Config c; c.geometry = geometry; c.stacks = 3;
        c.banks_per_channel = geometry == GeometryProfile::OCPReference ? 16 : 32;
        c.planes_per_bank = geometry == GeometryProfile::OCPReference ? 2 : 1;
        for (int stripe : {4096, 8192, 16384}) {
            c.host_stripe_bytes = stripe; validate_config(c);
            for (int i = 0; i < 10000; ++i) {
                const auto address = random() % (c.stacks * (512ULL << 30));
                require(encode_address(decode_address(address, 0, c), 0, c) == address, "mapping inversion");
            }
        }
        c.stacks = 1; c.host_stripe_bytes = 4096;
        Controller device(c);
        device.preload_region(0, 512ULL << 30);
        require(device.try_submit({0, page(c, 0), 64, Operation::Read, 7}, 0), "read admission");
        require(device.try_submit({1, page(c, 0, 1), 64, Operation::Read, 7}, 0), "read admission");
        require(device.try_submit({2, page(c, 1), 64, Operation::Read, 8}, 0), "read admission");
        require(device.advance(4000000).empty(), "response preceded channel transfer");
        require(device.advance(5000000).size() == 2, "independent Bank overlap or same-Bank serialization");
        const auto rest = device.advance(9000000);
        require(rest.size() == 1 && rest[0].request_id == 1 && device.pending_requests() == 0, "read conservation");
        require(device.try_submit({3, page(c, 0, 1), 64, Operation::Read}, 9000000), "cache hit admission");
        require(device.advance(9010000).size() == 1 && device.statistics().senses == 3, "cache hit sensed array");
        const auto& stats = device.statistics();
        require(stats.accepted_requests == stats.completed_requests && stats.failed_requests == 0, "read accounting");
        require(stats.host_read_bytes == 256 && stats.sensed_bytes == 3 * 4096, "host/array traffic distinction");
    }
}
void program_legality_and_backpressure() {
    Config c; c.channel_queue_capacity = 64; c.allow_program = true;
    // Deliberately synthetic test assumptions; no production erase timing is supplied.
    c.erase_latency_us = 10; c.write_timeout_us = 10;
    Controller device(c);
    require(device.try_submit({0, page(c, 0), 4096, Operation::Write}, 0), "page admission");
    require(!device.try_submit({1, page(c, 1), 64, Operation::Read}, 0), "queue backpressure");
    require(device.advance(80000000).empty(), "posted program acknowledgement");
    auto done = device.advance(100000000);
    require(done.size() == 1 && done[0].error == Error::None, "fresh page program");
    device.try_submit({2, page(c, 0, 2), 4096, Operation::Write}, 100000000);
    done = device.advance(110000000);
    require(done.size() == 1 && done[0].error == Error::IllegalProgramOrder, "skipped page accepted");
    device.try_submit({3, page(c, 1), 64, Operation::Write}, 110000000);
    done = device.advance(130000000);
    require(done.size() == 1 && done[0].error == Error::IncompletePageTimeout, "partial page timeout");
    require(device.statistics().programs == 1 && device.statistics().erases == 1, "failed writes changed NAND");
    require(device.pending_requests() == 0 && device.statistics().accepted_requests == 3, "admission conservation");
    rejects([&] { device.advance(0); });
    Config invalid; invalid.banks_per_channel = 1000000;
    rejects([&] { Controller bad(invalid); });
    invalid = Config{}; invalid.allow_program = true;
    rejects([&] { Controller bad(invalid); });
}
}
int main() {
    try {
        mapping_and_read_service(); program_legality_and_backpressure();
        std::cout << "HBFine standalone device contract passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
