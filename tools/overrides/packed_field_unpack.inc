if (guest_pc != 0x001A93A8u || trace_guest_pc_count != 0u || *budget_io == 0u)
    return DQ8_OVERRIDE_CALL_ORIGINAL;

/* This is the 25-instruction hot field-unpack loop in 0x001A93A8..0x001A9408.
 * Keep the generated dispatcher as the fallback whenever A05/trace boundaries
 * are present. The boundary set is immutable after startup; dynamic PC tracing
 * is rejected above. */
static int dq8_unpack_boundaries_checked;
static int dq8_unpack_boundaries_safe;
if (!dq8_unpack_boundaries_checked) {
    dq8_unpack_boundaries_safe = 1;
    for (uint32_t probe = 0x001A93A8u; probe <= 0x001A9408u; probe += 4u) {
        if (dq8_pc_needs_boundary(probe)) {
            dq8_unpack_boundaries_safe = 0;
            break;
        }
    }
    dq8_unpack_boundaries_checked = 1;
}
if (!dq8_unpack_boundaries_safe) return DQ8_OVERRIDE_CALL_ORIGINAL;

unsigned spent = 1u; /* entry instruction is accounted by translated_from */
uint32_t last_pc = guest_pc;
int first_iteration = 1;
#define DQ8_UNPACK_RETURN(next_pc) do { \
    *pc_io = (next_pc); \
    *budget_io -= spent - 1u; \
    return DQ8_OVERRIDE_HANDLED; \
} while (0)
#define DQ8_UNPACK_ADVANCE(next_pc) do { \
    if (spent >= *budget_io) DQ8_UNPACK_RETURN(next_pc); \
    previous_guest_pc = last_pc; \
    current_guest_pc = (next_pc); \
    if (r == main_guest_regs) main_guest_pc = (next_pc); \
    last_pc = (next_pc); \
    ++spent; \
    ++*steps_io; \
} while (0)

for (;;) {
    if (first_iteration) {
        if ((r[3] >> 25) == 3u) {
            return DQ8_OVERRIDE_CALL_ORIGINAL;
        }
        first_iteration = 0;
    } else {
        /* The next loop-head instruction is still unexecuted. Let the original
         * case handle the stream sentinel after returning to the dispatcher. */
        if ((r[3] >> 25) == 3u) DQ8_UNPACK_RETURN(0x001A93A8u);
        DQ8_UNPACK_ADVANCE(0x001A93A8u);
    }

    r[4] = r[3] >> 25;                         /* 0x001A93A8 */
    DQ8_UNPACK_ADVANCE(0x001A93ACu);
    {
        uint32_t a = r[4], b = 3u, t = a - b; /* cmp at 0x001A93AC */
        *z_io = (t == 0u); *n_io = (int)(t >> 31);
        *c_io = (a >= b); *v_io = (int)(((a ^ b) & (a ^ t)) >> 31);
    }
    DQ8_UNPACK_ADVANCE(0x001A93B0u);
    if (*z_io) DQ8_UNPACK_RETURN(0x001A92C0u);

    DQ8_UNPACK_ADVANCE(0x001A93B4u);
    r[4] = r[3] >> 20;
    DQ8_UNPACK_ADVANCE(0x001A93B8u);
    r[4] = r[11] + (r[4] << 1);
    DQ8_UNPACK_ADVANCE(0x001A93BCu);
    r[4] = dq8_mem16_fast(r[4]);
    DQ8_UNPACK_ADVANCE(0x001A93C0u);
    r[5] = r[4] & 0xFu;
    DQ8_UNPACK_ADVANCE(0x001A93C4u);
    r[4] >>= 4;
    DQ8_UNPACK_ADVANCE(0x001A93C8u);
    r[6] = r[4] & 0x1Fu;
    DQ8_UNPACK_ADVANCE(0x001A93CCu);
    r[4] >>= 5;
    DQ8_UNPACK_ADVANCE(0x001A93D0u);
    {
        unsigned amount = r[5] & 255u;
        uint32_t shifted = arm_shift(r[3], 0u, amount);
        *c_io = arm_shift_carry(r[3], 0u, amount, *c_io);
        r[3] = shifted; *z_io = (shifted == 0u); *n_io = (int)(shifted >> 31);
    }
    DQ8_UNPACK_ADVANCE(0x001A93D4u);
    if (*c_io) r[6] = 0u - r[6];
    DQ8_UNPACK_ADVANCE(0x001A93D8u);
    {
        uint32_t a = r[2], b = r[5], t = a - b;
        r[2] = t; *z_io = (t == 0u); *n_io = (int)(t >> 31);
        *c_io = (a >= b); *v_io = (int)(((a ^ b) & (a ^ t)) >> 31);
    }
    if (*n_io != *v_io) DQ8_UNPACK_RETURN(0x001A93DCu);
    DQ8_UNPACK_ADVANCE(0x001A93DCu); /* conditional branch, not taken */
    DQ8_UNPACK_ADVANCE(0x001A93E0u);
    r[5] = r[4] & 0x3Fu;
    DQ8_UNPACK_ADVANCE(0x001A93E4u);
    r[4] >>= 6;
    DQ8_UNPACK_ADVANCE(0x001A93E8u);
    r[12] += r[5] << 2;
    DQ8_UNPACK_ADVANCE(0x001A93ECu);
    r[8] = dq8_mem32_fast(r[12]); r[12] += 4u;
    DQ8_UNPACK_ADVANCE(0x001A93F0u);
    r[5] = r[8] & 0xFFu;
    DQ8_UNPACK_ADVANCE(0x001A93F4u);
    r[7] = r[8] >> 8;
    DQ8_UNPACK_ADVANCE(0x001A93F8u);
    r[7] = r[6] * r[7];
    DQ8_UNPACK_ADVANCE(0x001A93FCu);
    r[8] = r[0] + 0x1B8u;
    DQ8_UNPACK_ADVANCE(0x001A9400u);
    dq8_put32_fast(r[8] + (r[5] << 2), r[7]);
    DQ8_UNPACK_ADVANCE(0x001A9404u);
    {
        uint32_t t = r[4] & 1u;
        *z_io = (t == 0u); *n_io = (int)(t >> 31);
    }
    DQ8_UNPACK_ADVANCE(0x001A9408u);
    if (*z_io) continue;                    /* loop branch taken */
    DQ8_UNPACK_RETURN(0x001A940Cu);        /* return/pop remains original */
}
#undef DQ8_UNPACK_ADVANCE
#undef DQ8_UNPACK_RETURN
