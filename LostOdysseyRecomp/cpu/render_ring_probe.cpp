// Diagnostic: watch the UE3 rendering command ring for the races behind render-thread crashes.
//
// Two macOS/Metal battle crashes ran a rendering command against dead memory from the dispatch
// in RenderingThreadMain (0x824856A0, vtable slot +4 at 0x824857D8): once a pure virtual call
// (an already executed and destructed command run again), once FAddPrimitiveCommand
// (0x825AFD00 -> FScene::AddPrimitive 0x823B9168) with a scene whose primitive array was garbage.
// Soaks later caught committed 16-byte commands whose first word (the vtable) was 0 or float data
// while the following fields looked valid.
//
// GRenderCommandBuffer (0x8336A7A4) is a lock-free single-producer/single-consumer ring:
//   +0 start, +4 capacity end, +8 write, +12 end of data after the writer wrapped,
//   +16 writer allocation in progress, +20 read, +24 alignment.
// Every inlined ENQUEUE calls AllocationContext (0x82290AB8), which spins while the ring is
// full, and commits by advancing +8 and clearing +16. The rendering thread peeks with
// 0x82485CD8 before each command. The ring is only unsafe with a second writer or reader, and a
// lagging rendering thread makes every lifetime race wider. This probe records:
//   - concurrent or unfinished writer allocations (+16 already set) and writer thread changes,
//   - reader thread changes, a command pointer peeked twice, and implausible vtables before
//     they execute,
//   - for an implausible command: which enqueue site allocated that address and how long ago,
//     and whether a plausible vtable appears within 50 ms (late publication) or never (corruption),
//   - how far the rendering thread lags (queued bytes, writer full-ring waits),
//   - the last commands dispatched, printed by the crash handler on a fault.
// Opt out with LO_RENDER_RING_PROBE=0. Guest behaviour is unchanged except that an implausible
// command delays the rendering thread by at most 50 ms while it is observed.

#include <stdafx.h>
#include <os/logger.h>
#include <os/log_file.h>
#include <os/crash_handler.h>
#include <kernel/memory.h>

extern "C" PPC_FUNC(__imp__sub_82290AB8); // FRingBuffer::AllocationContext(ctx, ring, size)
extern "C" PPC_FUNC(__imp__sub_82485CD8); // FRingBuffer::BeginRead(ring, &data, &size) for GRenderCommandBuffer

namespace
{
    constexpr uint32_t kRenderRing = 0x8336A7A4;
    constexpr uint32_t kImageBegin = 0x82000000;
    constexpr uint32_t kImageEnd = 0x833C0000;

    struct CommandRecord
    {
        uint32_t data = 0, vtable = 0, execute = 0, read = 0, write = 0, end = 0, contiguous = 0, thread = 0;
    };

    struct AllocationRecord
    {
        std::atomic<uint32_t> address{ 0 }, size{ 0 }, caller{ 0 }, thread{ 0 };
        std::atomic<int64_t> nanoseconds{ 0 };
    };

    constexpr size_t kHistory = 48;
    CommandRecord g_history[kHistory];
    std::atomic<uint32_t> g_historyNext{ 0 };

    constexpr size_t kAllocations = 64;
    AllocationRecord g_allocations[kAllocations];
    std::atomic<uint32_t> g_allocationNext{ 0 };

    std::atomic<uint32_t> g_nextThreadTag{ 0 };
    std::atomic<uint32_t> g_writerThread{ 0 }, g_readerThread{ 0 };
    std::atomic<uint32_t> g_maxQueued{ 0 }, g_writerWaits{ 0 }, g_maxWaitMs{ 0 }, g_commands{ 0 };

    bool Enabled()
    {
        static const bool enabled = [] {
            const char* value = getenv("LO_RENDER_RING_PROBE");
            return !value || strcmp(value, "0") != 0;
        }();
        return enabled;
    }

    uint32_t ThreadTag()
    {
        thread_local const uint32_t tag = g_nextThreadTag.fetch_add(1) + 1;
        return tag;
    }

    int64_t NowNanoseconds()
    {
        return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    bool InImage(uint32_t address)
    {
        return address >= kImageBegin && address < kImageEnd && (address & 3) == 0;
    }

    // Any guest address above the null guard and below the E alias (unmapped with 16 KiB pages).
    // GRenderCommandBuffer itself lives low (0x700000), so do not restrict to the heap ranges.
    bool Readable(uint32_t address, uint32_t size)
    {
        return address >= 0x10000 && address < 0xE0000000 && address + size < 0xE0000000;
    }

    struct RingState
    {
        uint32_t start, capacity, write, end, busy, read, align;
    };

    RingState ReadRing(uint8_t* base)
    {
        return { PPC_LOAD_U32(kRenderRing), PPC_LOAD_U32(kRenderRing + 4), PPC_LOAD_U32(kRenderRing + 8),
            PPC_LOAD_U32(kRenderRing + 12), PPC_LOAD_U32(kRenderRing + 16), PPC_LOAD_U32(kRenderRing + 20),
            PPC_LOAD_U32(kRenderRing + 24) };
    }

    uint32_t Queued(const RingState& ring)
    {
        if (ring.write >= ring.read)
            return ring.write - ring.read;
        return (ring.end - ring.read) + (ring.write - ring.start);
    }

    std::string FormatHistory(size_t count)
    {
        std::string text;
        const uint32_t next = g_historyNext.load();
        for (size_t i = std::min<size_t>(count, std::min<size_t>(next, kHistory)); i > 0; --i)
        {
            const auto& r = g_history[(next - i) % kHistory];
            text += fmt::format("\n  -{:02} data={:#010x} vt={:#010x} exec={:#010x} read={:#010x} write={:#010x} end={:#010x} contiguous={} thread={}",
                i, r.data, r.vtable, r.execute, r.read, r.write, r.end, r.contiguous, r.thread);
        }
        return text;
    }

    // Which enqueue site allocated this address most recently, if it is still in the history.
    std::string DescribeAllocation(uint32_t address)
    {
        const uint32_t next = g_allocationNext.load();
        const int64_t now = NowNanoseconds();
        for (size_t i = 1; i <= std::min<size_t>(next, kAllocations); ++i)
        {
            const auto& a = g_allocations[(next - i) % kAllocations];
            if (a.address.load() == address)
                return fmt::format("allocated by caller={:#x} size={} thread={} {:.3f} ms before this peek",
                    a.caller.load(), a.size.load(), a.thread.load(), (now - a.nanoseconds.load()) / 1e6);
        }
        return "allocation not in the recent history";
    }

    // Async-signal context: fixed buffer, no allocation or formatting library.
    void CrashReport() noexcept
    {
        char line[160];
        auto hex = [](char* out, uint32_t value) {
            const char digits[] = "0123456789ABCDEF";
            for (int i = 7; i >= 0; --i) *out++ = digits[(value >> (i * 4)) & 15];
            return out;
        };
        auto write = [&](const char* prefix, const uint32_t* values, size_t count) {
            char* out = line;
            for (const char* p = prefix; *p;) *out++ = *p++;
            for (size_t i = 0; i < count; ++i) { *out++ = ' '; out = hex(out, values[i]); }
            *out++ = '\n';
            os::logger::EmergencyWrite(line, size_t(out - line));
        };
        uint8_t* base = g_memory.base;
        if (!base)
            return;
        const RingState ring = ReadRing(base);
        const uint32_t ringValues[] = { ring.start, ring.capacity, ring.write, ring.end, ring.busy, ring.read, ring.align,
            g_writerThread.load(), g_readerThread.load(), g_maxQueued.load() };
        write("[crash] render ring start/capacity/write/end/busy/read/align writer reader maxQueued:", ringValues, std::size(ringValues));
        const uint32_t next = g_historyNext.load();
        const size_t count = std::min<size_t>(next, 24);
        for (size_t i = count; i > 0; --i)
        {
            const auto& r = g_history[(next - i) % kHistory];
            const uint32_t values[] = { r.data, r.vtable, r.execute, r.read, r.write, r.end, r.contiguous, r.thread };
            write("[crash] render command data/vtable/execute/read/write/end/contiguous/thread:", values, std::size(values));
        }
        const uint32_t allocationNext = g_allocationNext.load();
        const size_t allocationCount = std::min<size_t>(allocationNext, 16);
        for (size_t i = allocationCount; i > 0; --i)
        {
            const auto& a = g_allocations[(allocationNext - i) % kAllocations];
            const uint32_t values[] = { a.address.load(), a.size.load(), a.caller.load(), a.thread.load() };
            write("[crash] render ring allocation address/size/caller/thread:", values, std::size(values));
        }
    }

    const bool g_registered = [] {
        RegisterCrashReporter(CrashReport);
        return true;
    }();
}

PPC_FUNC(sub_82290AB8)
{
    if (ctx.r4.u32 != kRenderRing || !Enabled())
    {
        __imp__sub_82290AB8(ctx, base);
        return;
    }

    const uint32_t caller = uint32_t(ctx.lr);
    const uint32_t size = ctx.r5.u32;
    const uint32_t context = ctx.r3.u32;
    const uint32_t self = ThreadTag();
    const uint32_t previous = g_writerThread.exchange(self);
    if (PPC_LOAD_U32(kRenderRing + 16) != 0)
    {
        static std::atomic<uint32_t> reports{ 0 };
        if (reports.fetch_add(1) < 16)
            LOG_ERROR("render ring: allocation while another is uncommitted (thread {} after thread {}, caller={:#x}, size={}) - concurrent writer or missing commit",
                self, previous, caller, size);
    }
    if (previous && previous != self)
    {
        static std::atomic<uint32_t> reports{ 0 };
        if (reports.fetch_add(1) < 16)
            LOG_WARNING("render ring: writer thread changed {} -> {} (caller={:#x})", previous, self, caller);
    }

    const auto begin = std::chrono::steady_clock::now();
    __imp__sub_82290AB8(ctx, base);
    const auto waitedMs = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count());

    // AllocationContext stores the allocation at context+4 and the aligned size at context+8.
    auto& record = g_allocations[g_allocationNext.fetch_add(1) % kAllocations];
    record.address.store(PPC_LOAD_U32(context + 4));
    record.size.store(PPC_LOAD_U32(context + 8));
    record.caller.store(caller);
    record.thread.store(self);
    record.nanoseconds.store(NowNanoseconds());

    if (waitedMs >= 16)
    {
        g_writerWaits.fetch_add(1);
        uint32_t max = g_maxWaitMs.load();
        while (waitedMs > max && !g_maxWaitMs.compare_exchange_weak(max, waitedMs)) {}
        static std::atomic<uint32_t> reports{ 0 };
        if (reports.fetch_add(1) < 8)
            LOG_WARNING("render ring: full, writer waited {} ms for {} bytes (caller={:#x}, queued={} bytes)",
                waitedMs, size, caller, Queued(ReadRing(base)));
    }
}

PPC_FUNC(sub_82485CD8)
{
    const uint32_t dataAddress = ctx.r4.u32;
    __imp__sub_82485CD8(ctx, base);
    if (!Enabled() || ctx.r3.u32 == 0)
        return;

    const uint32_t self = ThreadTag();
    const uint32_t previousReader = g_readerThread.exchange(self);
    if (previousReader && previousReader != self)
        LOG_WARNING("render ring: reader thread changed {} -> {}", previousReader, self);

    const RingState ring = ReadRing(base);
    CommandRecord record;
    record.data = PPC_LOAD_U32(dataAddress);
    record.read = ring.read;
    record.write = ring.write;
    record.end = ring.end;
    record.contiguous = PPC_LOAD_U32(ctx.r5.u32);
    record.thread = self;
    if (Readable(record.data, 8))
    {
        record.vtable = PPC_LOAD_U32(record.data);
        if (InImage(record.vtable))
            record.execute = PPC_LOAD_U32(record.vtable + 4);
    }

    const uint32_t next = g_historyNext.load();
    const CommandRecord* last = next ? &g_history[(next - 1) % kHistory] : nullptr;
    const bool repeated = last && last->data == record.data && last->thread == self;
    const bool implausible = !InImage(record.vtable) || !InImage(record.execute);
    g_history[next % kHistory] = record;
    g_historyNext.store(next + 1);

    if (repeated || implausible)
    {
        static std::atomic<uint32_t> reports{ 0 };
        if (reports.fetch_add(1) < 8)
        {
            std::string payload;
            if (Readable(record.data, 32))
                for (uint32_t offset = 0; offset < 32; offset += 4)
                    payload += fmt::format(" {:08x}", PPC_LOAD_U32(record.data + offset));
            const std::string allocation = DescribeAllocation(record.data);

            // Late publication shows a plausible vtable appearing shortly; corruption never does.
            std::string settle = "not observed";
            if (implausible && Readable(record.data, 8))
            {
                const auto watchStart = std::chrono::steady_clock::now();
                settle = "vtable still implausible after 50 ms";
                while (std::chrono::steady_clock::now() - watchStart < std::chrono::milliseconds(50))
                {
                    const uint32_t vtable = PPC_LOAD_U32(record.data);
                    if (vtable != record.vtable)
                    {
                        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - watchStart).count();
                        const uint32_t execute = InImage(vtable) ? PPC_LOAD_U32(vtable + 4) : 0;
                        settle = fmt::format("vtable changed to {:#x} (exec {:#x}, {}) after {:.3f} ms",
                            vtable, execute, InImage(vtable) && InImage(execute) ? "plausible" : "still implausible", ms);
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::microseconds(100));
                }
            }

            LOG_ERROR("render ring: {} command data={:#x} vt={:#x} exec={:#x} ring start={:#x} capacity={:#x} write={:#x} end={:#x} busy={} read={:#x} payload:{} | {} | {}{}",
                repeated ? "same" : "implausible", record.data, record.vtable, record.execute, ring.start, ring.capacity,
                ring.write, ring.end, ring.busy, ring.read, payload, allocation, settle, FormatHistory(16));
        }
    }

    const uint32_t queued = Queued(ring);
    uint32_t max = g_maxQueued.load();
    while (queued > max && !g_maxQueued.compare_exchange_weak(max, queued)) {}

    g_commands.fetch_add(1);
    static auto lastReport = std::chrono::steady_clock::now();
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport >= std::chrono::seconds(30))
    {
        lastReport = now;
        LOG_INFO("render ring: {} commands in 30 s, max queued {} KB of {} KB, writer full-ring waits {} (max {} ms)",
            g_commands.exchange(0), g_maxQueued.exchange(0) / 1024, (ring.capacity - ring.start) / 1024,
            g_writerWaits.exchange(0), g_maxWaitMs.exchange(0));
    }
}
