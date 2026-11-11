/* Bounded trial on the IN2010 MMU-500. Caller holds hk_smmu_lock, has a
 * pinned, cache-clean arena, and keeps the sensor processor stopped. */
#ifndef SLPI_DMA_ROUTE_H
#define SLPI_DMA_ROUTE_H
struct slpi_route_io {
    void *context;
    uint32_t (*read)(void *, unsigned);
    void (*write)(void *, unsigned, uint32_t); /* includes device barrier */
    bool (*flush_asid6)(void *);
};
struct slpi_route_result {
    bool attempted, enabled, mapped, restored, reclaimable;
    unsigned failed_at, rollback_at;
    uint32_t expected, observed, rollback_expected, rollback_observed;
};
struct slpi_bank_setting {
    unsigned offset;
    uint32_t value, check_mask;
};

static inline bool
slpi_route_readback(const struct slpi_route_io *io, struct slpi_route_result *r,
                    unsigned reg, uint32_t expected, uint32_t mask, bool rollback)
{
    uint32_t value = io->read(io->context, reg);
    if (!((value ^ expected) & mask)) return true;
    if (rollback) {
        r->rollback_at = reg;
        r->rollback_expected = expected;
        r->rollback_observed = value;
    } else {
        r->failed_at = reg;
        r->expected = expected;
        r->observed = value;
    }
    return false;
}

/* Only the specific MMU geometry and address range already observed here.
 * CB6 has been exercised by the native-board Wi-Fi driver. Never search
 * arbitrary context banks (some belong to the secure display). */
static inline bool
slpi_route_slot(const struct slpi_route_io *io, unsigned display_cb, unsigned *slot)
{
    void *c = io->context;
    if (display_cb == 6 || io->read(c, 0x20) != 0x4c017e57 ||
        io->read(c, 0x24) != 0x6000004a || (io->read(c, 0) & 0x409) != 0x400)
        return false;
    unsigned free_slot = 87;
    for (unsigned i = 0; i < 87; i++) {
        uint32_t smr = io->read(c, 0x800 + 4*i);
        uint32_t route = io->read(c, 0xc00 + 4*i);
        if (!(smr >> 31)) {
            if (free_slot == 87 && !smr && route == 0x200ff) free_slot = i;
            continue;
        }
        if ((route & 255) == 6) return false;
        for (unsigned sid = 0x541; sid <= 0x543; sid++)
            if (((sid ^ smr) & ~(smr >> 16) & 0x7fff) == 0) return false;
    }
    if (free_slot == 87 || (io->read(c, 0x86000) & 1) ||
        (io->read(c, 0x86058) & 0xc00001feu) || io->read(c, 0x48) ||
        io->read(c, 0x86020) || io->read(c, 0x86024)) return false;
    *slot = free_slot;
    return true;
}

static inline struct slpi_route_result
slpi_route_run(const struct slpi_route_io *io, unsigned display_cb, uint64_t root_pa, bool connect_stream,
               bool (*operate_until_stopped)(void *), void *owner)
{
    struct slpi_route_result result = { .restored = true, .reclaimable = true };
    unsigned slot;
    if (!root_pa || (root_pa & ~UINT64_C(0xffffff000)) ||
        !slpi_route_slot(io, display_cb, &slot)) return result;
    void *c = io->context;
    const unsigned ctl = 0x86000, smr = 0x800 + 4*slot, s2cr = 0xc00 + 4*slot;
    const uint32_t old_ctl = io->read(c, ctl);
    const struct slpi_bank_setting settings[] = {
        {0x1818, 1, ~0u}, {0x1018, 0x1f300, ~0u},
        {0x86010, 0x38011, ~0x60u}, /* RES1 in native readback */
        {0x86030, 0x803519, ~0u},
        {0x86020, (uint32_t)root_pa, ~0u},
        {0x86024, (6u << 16) | (uint32_t)(root_pa >> 32), ~0u},
        {0x86028, 0, ~0u}, {0x8602c, 0, ~0u},
        {0x86038, 0xf404ff44, ~0u}, {0x8603c, 0, ~0u}
    };
    enum { N = sizeof(settings)/sizeof(settings[0]) };
    uint32_t saved[N];
    for (unsigned i = 0; i < N; i++) saved[i] = io->read(c, settings[i].offset);
    unsigned touched = 0;
    bool switched = false, route_written = false;
    result.attempted = true;
    result.restored = false;
    result.reclaimable = false;
    io->write(c, ctl, 0);
    if (io->read(c, ctl) & 1) {result.failed_at = ctl; goto rollback;}
    for (unsigned i = 0; i < N; i++) {
        const struct slpi_bank_setting *s = &settings[i];
        touched = i + 1;
        io->write(c, s->offset, s->value);
        if (!slpi_route_readback(io, &result, s->offset, s->value, s->check_mask, false)) {
            result.failed_at = s->offset;
            goto rollback;
        }
    }
    if (!io->flush_asid6(c)) {result.failed_at = 0x867f4; goto rollback;}
    switched = true;
    io->write(c, ctl, 0x1127);
    if ((io->read(c, ctl) & ~0x1000u) != 0x127) {result.failed_at = ctl; goto rollback;}
    result.enabled = true;
    if (!slpi_route_readback(io, &result, 0x86058, 0, 0xc00001feu, false) ||
        !slpi_route_readback(io, &result, 0x48, 0, ~0u, false) || !connect_stream)
        goto rollback;
    route_written = true;
    io->write(c, s2cr, 6);
    if (io->read(c, s2cr) != 6) {result.failed_at = s2cr; goto rollback;}
    io->write(c, smr, 0x80000541);
    result.mapped = slpi_route_readback(io, &result, smr, 0x80000541, ~0u, false) &&
        slpi_route_readback(io, &result, 0x86058, 0, 0xc00001feu, false) &&
        slpi_route_readback(io, &result, 0x48, 0, ~0u, false);
    if (result.mapped && operate_until_stopped) {
        /* The mapping is owned until the peer has been positively stopped,
         * even on timeout, malformed response or cancelled transaction. */
        if (!operate_until_stopped(owner)) return result;
        if (!slpi_route_readback(io,&result,smr,0x80000541,~0u,false) ||
            !slpi_route_readback(io,&result,s2cr,6,~0u,false) ||
            !slpi_route_readback(io,&result,0x86020,(uint32_t)root_pa,~0u,false) ||
            !slpi_route_readback(io,&result,0x86024,(6u<<16)|(uint32_t)(root_pa>>32),~0u,false))
            return result;
    }
rollback:
    if (route_written) {
        io->write(c, smr, 0);
        if (!slpi_route_readback(io, &result, smr, 0, ~0u, true)) return result;
        io->write(c, s2cr, 0x200ff);
        if (!slpi_route_readback(io, &result, s2cr, 0x200ff, ~0u, true)) return result;
    }
    if (switched) {
        io->write(c, ctl, 0);
        if (!slpi_route_readback(io, &result, ctl, 0, 1, true)) return result;
    }
    if (touched && !io->flush_asid6(c)) {result.rollback_at = 0x867f4; return result;}
    /* Context configuration uses CBA2R before CBAR, just as on setup.
     * Restore bank-local registers before switching its format back. */
    unsigned configured = touched;
    while (touched > 2) {
        unsigned i = --touched;
        io->write(c, settings[i].offset, saved[i]);
    }
    for (unsigned i = 0; i < configured && i < 2; i++)
        io->write(c, settings[i].offset, saved[i]);
    io->write(c, ctl, old_ctl);
    /* OnePlus destroys a private domain by disabling its bank before freeing
     * tables. Here also require confirmed TLB completion, both translation
     * bases restored, every route detached, and all other saved state intact.
     * The only admitted residual state is the observed inactive S1 CBAR. */
    bool exact = true, inert = true;
    for (unsigned i = 0; i < N; i++) {
        uint32_t value = io->read(c, settings[i].offset);
        if (value == saved[i]) continue;
        if (exact) {
            result.rollback_at = settings[i].offset;
            result.rollback_expected = saved[i];
            result.rollback_observed = value;
        }
        exact = false;
        if (!(settings[i].offset == 0x1018 && saved[i] == 0 && value == 0x1f300))
            inert = false;
    }
    if (!slpi_route_readback(io, &result, ctl, old_ctl, ~0u, true) ||
        !slpi_route_readback(io, &result, smr, 0, ~0u, true) ||
        !slpi_route_readback(io, &result, s2cr, 0x200ff, ~0u, true)) return result;
    for (unsigned i = 0; i < 87; i++) {
        uint32_t active = io->read(c, 0x800 + 4*i);
        if (!(active >> 31)) continue;
        if ((io->read(c, 0xc00 + 4*i) & 255) == 6 ||
            ((active ^ 0x541) & ~(active >> 16) & 0x7fff) == 0) return result;
    }
    result.restored = exact;
    result.reclaimable = inert && !(io->read(c, 0x86000) & 1) &&
        io->read(c, 0x86020) == 0 && io->read(c, 0x86024) == 0 &&
        !(io->read(c, 0x86058) & 0xc00001feu) && !io->read(c, 0x48);
    return result;
}
static inline struct slpi_route_result
slpi_route_trial(const struct slpi_route_io *io, unsigned display_cb, uint64_t root_pa, bool connect_stream)
{
    return slpi_route_run(io, display_cb, root_pa, connect_stream, NULL, NULL);
}
#endif
