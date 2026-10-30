# HBFine

HBFine is a portable C++20 timing model for HBF Host Channels, Banks, Planes,
page buffers, and NAND programming. It has no dependency on GTSim, Ramulator,
CUDA, Python, YAML parsers, or a GPU clock. It models timing and request identity;
it does not store tensor values or implement a host allocator, FTL, GC, wear
policy, or numerical computation. Distributed under the [MIT license](LICENSE).

## Build and consume

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --prefix /your/install/prefix
```

An embedding project can use either `add_subdirectory(path/to/hbfine)` or
`find_package(HBFine CONFIG REQUIRED)`, then link `HBFine::hbfine`. Public headers
are under `hbfine/`; the C++ namespace is `hbfine`. Standalone tests default on
when this is the top-level project; set `HBFINE_BUILD_TESTS` explicitly to change
that. There are no network fetches or embedding-project source includes.

```cpp
#include <hbfine/controller.hpp>
hbfine::Controller device(hbfine::Config{});
device.preload_region(0, 4096); // valid input, cold cache, no timed programming
if (device.try_submit({1, 0, 64, hbfine::Operation::Read}, 0)) {
    auto completions = device.advance(5000000); // 5 microseconds, in picoseconds
}
```

`try_submit(request, now_ps)` rejects capacity pressure with `false`, retaining
caller ownership. Invalid requests/configurations throw. Accepted requests yield
exactly one completion from `advance(now_ps)`, which may jump across many events.
Both calls require monotonic physical time. Request IDs must be unique until
completion; explicit AXI IDs are 14-bit, or default to `request_id % 16384`.
Same-ID responses are ordered within each physical channel and read/write
interface independently. Different IDs may complete out of order. The instance
is single-threaded and never calls back into the embedding simulator.

`pending_requests()` includes accepted parents and undelivered completions.
`statistics()` exposes traffic, failed requests, Bank cache behavior, queuing,
and per-resource occupancy. Idle excludes harmless stale timeout events after a
page has completed, just as periodic device maintenance need not block a host
drain. There is no per-cycle sweep over all Banks/Planes.

## Model boundaries and provenance

- [OCP HBF v0.7.0](https://www.opencompute.org/documents/ocp-hbf-architecture-specification-v0-7-0-final-pdf),
  §§4–5: physical Host Channels, virtual AXI ports, Bank-local sense order,
  two 4KiB Bank buffers, 64B reads, accumulated 4KiB non-posted writes, and
  page-0 erase/sequential programming. Buffers never add sense engines.
- [HBFlex, §3 and §6.1](https://arxiv.org/html/2609.18675v1): research defaults
  are 16 dies × 32 Planes × 512 blocks × 512 pages × 4KiB = 512GiB per stack;
  4us sense, 75us program, 488/27.2GB/s read/write targets and 40MB base-die SRAM.
- The mapping between these two hierarchies is an explicit assumption.
  `HBFlexResearch` uses 32 Banks/Host Channel and one Plane/Bank (512 independent
  sense engines/stack). `OCPReference` uses 16 Banks/Host Channel and two
  Planes/Bank (256 serialized engines/stack). Both have 512 Planes/stack and
  exactly one sense/program/erase engine per Bank. No multi-plane command is
  inferred. Their cold-array ceilings are 524.288 and 262.144GB/s respectively.

Validated geometry is deliberately limited to those two declared mappings.
Address helpers require a validated configuration and page-aligned device base.
Whole-page host striping is reversible; virtual AXI ports add no bandwidth.
Read hits may use a ready buffer while another page senses; pinned/filling
buffers cannot be evicted. New misses use the Bank FIFO. Per-channel read and
write serializers share their respective per-stack target equally. Dispatch
latency/interval, per-die transfer bandwidth, and stack ECC bandwidth are optional
explicit parameters; zero disables the latter two, rather than inventing a
measurement. A Core Die transfer/ECC reservation is one whole page.

`ucie_ceiling_GBps_per_stack` constrains target validation; the embedding system
must model the external UCIe path separately. OCP Grade-2's 1536GB/s link ceiling
is not a NAND-array bandwidth. Base-die SRAM is a separate capacity constraint
for write assembly; no read-side SRAM latency-hiding cache is modeled.

Programming defaults **disabled** because the cited sources do not supply an
erase duration or host timeout. Enabling it requires both values explicitly.
Tests use labeled synthetic assumptions, never production calibration. A timeout
fails every accepted piece, including pieces still arriving in the serializer.
Only complete sequential pages in fresh blocks may program; block page 0 incurs
erase. A block containing any preloaded input is protected against programming,
even if a particular page was not preloaded. Updating existing data requires the
host to allocate another block; this model performs no relocation or GC. Program
acknowledgement follows Core Die completion. Error values distinguish uninitialized
reads, incomplete-page timeouts, and illegal program order.

Statistics count cache hits/misses per 64B beat, including coalesced beats waiting
on an in-flight sense. `senses` counts actual 4KiB NAND operations. Stack busy time
sums Bank sense/program/erase occupancy, so divide by the number of Banks when
reporting stack utilization. Channel read/write occupancies are independent;
Bank wait time is summed per array operation and transfer wait per reservation,
not a direct GPU stall-cycle counter.

## 8-Hi research geometry

`dies_per_stack=8` is an explicitly modeled scaling alternative. Keep 32 Planes
per die, 512 blocks per Plane, 512 pages per block and 4KiB pages: capacity is
256GiB/stack. `HBFlexResearch` then uses 16 Banks per Host Channel and one Plane
per Bank; `OCPReference` uses 8 Banks per Host Channel and two Planes per Bank.
The 16-Hi defaults and mappings remain unchanged. All addressing, physical die
indices and resource/statistics vectors scale with the chosen die count.

At 4us sense / 75us program, the 8-Hi HBFlex mapping has array ceilings of
262.144GB/s read and 13.981013GB/s program per stack. Host targets and the external
link ceiling remain separate explicit inputs. An embedding study may choose
262.144/13.98GB/s targets and a 384GB/s Grade-1 link ceiling; these are research
assumptions rather than a measured Grade-1 device guarantee. Buffer hits retain
their normal fast path; changing die count does not turn each hit into a new
4us sense. Input preloading marks NAND data valid and leaves page caches cold.
