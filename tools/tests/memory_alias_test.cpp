#include <kernel/guest_address_space.h>
#include <cstdio>
#include <cstdint>
#include <initializer_list>

int main()
{
    // 16 KiB-page hosts leave E unmapped (see UnmappedAliasRange); only check its
    // one-page offset where the host can actually express it.
    const auto unmapped = GuestAddressSpace::UnmappedAliasRange();
    const bool eMapped = unmapped.begin == unmapped.end;
    // Repeat to exercise release and reuse, including the preferred host base.
    for (int iteration = 0; iteration < 2; ++iteration)
    {
        uint8_t* base = GuestAddressSpace::Allocate();
        if (!base)
        {
            const auto failure = GuestAddressSpace::GetFailureInfo();
            std::fprintf(stderr, "Guest address space allocation failed: %s, error=%u view=%d size=%llu offset=%llu\n",
                         GuestAddressSpace::FailureOperationName(failure.operation), failure.error, failure.viewIndex,
                         static_cast<unsigned long long>(failure.size), static_cast<unsigned long long>(failure.offset));
            return 1;
        }
        auto word = [base](uint32_t address) -> volatile uint32_t& {
            return *reinterpret_cast<volatile uint32_t*>(base + address);
        };
        bool ok = true;
        for (uint32_t offset : {0u, 0x1000u, 0x1FFFCu, 0x1234560u, 0x1FFFFFFCu})
        {
            word(0xA0000000u + offset) = 0x12345678;
            ok &= word(0xC0000000u + offset) == 0x12345678;
            word(0xC0000000u + offset) = 0;
            ok &= word(0xA0000000u + offset) == 0;
            if (eMapped && offset >= 0x1000)
            {
                word(0xE0000000u + offset - 0x1000) = 0xABCDEF01;
                ok &= word(0xA0000000u + offset) == 0xABCDEF01;
                ok &= word(0xC0000000u + offset) == 0xABCDEF01;
            }
        }
        // A virtual page must not accidentally alias the physical page. Use the
        // first virtual allocator page (PageAllocator::Init): the guest null
        // guard covers a whole host page, which is 16 KiB on Apple Silicon.
        word(0x00100000) = 0x87654321;
        word(0xA0100000) = 0xDEADBEEF;
        ok &= word(0x00100000) == 0x87654321;

        // Occlusion-query round trip: CPU initializes through C, GPU writes
        // END through A, CPU subtracts BEGIN from END through C.
        constexpr uint32_t query = 0x01002000;
        word(0xC0000000 + query + 0x30) = 100;
        word(0xC0000000 + query + 0x10) = 0xFFFFFFFF;
        ok &= word(0xA0000000 + query + 0x10) == 0xFFFFFFFF;
        word(0xA0000000 + query + 0x10) = 65636;
        ok &= word(0xC0000000 + query + 0x10) - word(0xC0000000 + query + 0x30) == 65536;
        GuestAddressSpace::Release(base);
        if (!ok)
        {
            std::fprintf(stderr, "Physical alias coherence failed\n");
            return 1;
        }
    }
    std::puts(eMapped
        ? "PASS: A/C coherence, E offset, virtual isolation, query round trip, release/reallocate"
        : "PASS: A/C coherence, virtual isolation, query round trip, release/reallocate (E unmapped on this host)");
    return 0;
}
