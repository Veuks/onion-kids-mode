#ifndef KIDSMODE_ARTWORK_SCALER_H
#define KIDSMODE_ARTWORK_SCALER_H

#include <stdint.h>

// Only pixel arithmetic is accelerated. Sampling, surface ownership and
// orientation remain in scaleSurface. Define KIDSMODE_SCALAR_ARTWORK to
// compare with the portable path without changing the rest of the UI.
#if defined(__ARM_NEON) && !defined(KIDSMODE_SCALAR_ARTWORK)
#include <arm_neon.h>
#define KIDSMODE_ARTWORK_NEON 1
#else
#define KIDSMODE_ARTWORK_NEON 0
#endif

// Onion's shared build flags do not request optimization. Optimize this
// self-contained hot loop, not input handling or the rest of kidui.
#if defined(__GNUC__) && !defined(__clang__)
#define KIDSMODE_SCALER_OPT __attribute__((optimize("O2")))
#else
#define KIDSMODE_SCALER_OPT
#endif

static KIDSMODE_SCALER_OPT void artworkScaleRow(
    uint32_t *dst, const uint32_t *top_row, const uint32_t *bottom_row,
    int width, const int *x0_map, const int *x1_map, const int *wx_map, int wy)
{
    int x = 0;
    unsigned inv_y = 256 - wy;
#if KIDSMODE_ARTWORK_NEON
    const uint16x4_t weight_y = vdup_n_u16((uint16_t)wy);
    const uint16x4_t inverse_y = vdup_n_u16((uint16_t)inv_y);
    const uint32x4_t rounding = vdupq_n_u32(32768);
    const uint16x8_t full_weight = vdupq_n_u16(256);

    // Two ARGB pixels per iteration. Each pixel's four byte channels use
    // its own horizontal weight; vertical weights are shared by the row.
    // Explicit loads avoid alignment requirements and out-of-row reads.
    for (; x + 1 < width; x += 2) {
        int a0 = x0_map[x], a1 = x1_map[x];
        int b0 = x0_map[x + 1], b1 = x1_map[x + 1];
        uint8x8_t p00 = vcreate_u8((uint64_t)top_row[a0] |
                                  ((uint64_t)top_row[b0] << 32));
        uint8x8_t p01 = vcreate_u8((uint64_t)top_row[a1] |
                                  ((uint64_t)top_row[b1] << 32));
        uint8x8_t p10 = vcreate_u8((uint64_t)bottom_row[a0] |
                                  ((uint64_t)bottom_row[b0] << 32));
        uint8x8_t p11 = vcreate_u8((uint64_t)bottom_row[a1] |
                                  ((uint64_t)bottom_row[b1] << 32));
        uint16x8_t weight_x = vcombine_u16(
            vdup_n_u16((uint16_t)wx_map[x]),
            vdup_n_u16((uint16_t)wx_map[x + 1]));
        uint16x8_t inverse_x = vsubq_u16(full_weight, weight_x);
        uint16x8_t top = vaddq_u16(
            vmulq_u16(vmovl_u8(p00), inverse_x),
            vmulq_u16(vmovl_u8(p01), weight_x));
        uint16x8_t bottom = vaddq_u16(
            vmulq_u16(vmovl_u8(p10), inverse_x),
            vmulq_u16(vmovl_u8(p11), weight_x));

        // Horizontal sums fit in 16 bits (maximum 255*256). Widen before
        // the vertical multiply; round once exactly like the scalar path.
        uint32x4_t lo = vmlal_u16(
            vmull_u16(vget_low_u16(top), inverse_y),
            vget_low_u16(bottom), weight_y);
        uint32x4_t hi = vmlal_u16(
            vmull_u16(vget_high_u16(top), inverse_y),
            vget_high_u16(bottom), weight_y);
        uint8x8_t result = vmovn_u16(vcombine_u16(
            vshrn_n_u32(vaddq_u32(lo, rounding), 16),
            vshrn_n_u32(vaddq_u32(hi, rounding), 16)));
        vst1_u8((uint8_t *)(dst + x), result);
    }
#endif
    // Also handles the final pixel of odd-width images. No vector load
    // or store ever extends into padding or the next row.
    for (; x < width; x++) {
        int x0 = x0_map[x], x1 = x1_map[x];
        unsigned wx = wx_map[x], inv_x = 256 - wx;
        uint32_t p00 = top_row[x0], p01 = top_row[x1];
        uint32_t p10 = bottom_row[x0], p11 = bottom_row[x1];
        uint32_t result = 0;
        for (int shift = 0; shift <= 24; shift += 8) {
            unsigned top = ((p00 >> shift) & 0xFF) * inv_x +
                           ((p01 >> shift) & 0xFF) * wx;
            unsigned bottom = ((p10 >> shift) & 0xFF) * inv_x +
                              ((p11 >> shift) & 0xFF) * wx;
            unsigned value = (top * inv_y + bottom * wy + 32768) >> 16;
            result |= (value & 0xFF) << shift;
        }
        dst[x] = result;
    }
}

#undef KIDSMODE_SCALER_OPT
#endif
