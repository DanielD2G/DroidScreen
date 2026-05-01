/*
 * DroidScreen - H.264 SPS patcher for low-latency decoding
 *
 * Moonlight patches H.264 SPS units so decoders can allocate the minimum
 * reorder queue. For our VideoToolbox H.264 Baseline stream, rewrite the SPS
 * to advertise:
 *   - constrained-baseline compatible flags
 *   - num_ref_frames = 1
 *   - VUI bitstream restriction:
 *       num_reorder_frames = 0
 *       max_dec_frame_buffering = 1
 *
 * The rewriter is intentionally narrow: it handles Baseline/Constrained
 * Baseline SPS RBSPs. If parsing fails, it falls back to the old in-place
 * constraint flag patch.
 */

#ifndef DROIDSCREEN_SPS_PATCH_H
#define DROIDSCREEN_SPS_PATCH_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const uint8_t *data;
    size_t len;
    size_t bitpos;
    int ok;
} sps_bit_reader_t;

typedef struct {
    uint8_t *data;
    size_t cap;
    size_t bitpos;
    int ok;
} sps_bit_writer_t;

static inline void sps_br_init(sps_bit_reader_t *br,
                               const uint8_t *data,
                               size_t len) {
    br->data = data;
    br->len = len;
    br->bitpos = 0;
    br->ok = 1;
}

static inline uint32_t sps_br_read_bit(sps_bit_reader_t *br) {
    if (!br->ok || br->bitpos >= br->len * 8) {
        br->ok = 0;
        return 0;
    }
    size_t byte_pos = br->bitpos >> 3;
    int bit = 7 - (int)(br->bitpos & 7);
    br->bitpos++;
    return (br->data[byte_pos] >> bit) & 1;
}

static inline uint32_t sps_br_read_bits(sps_bit_reader_t *br, int count) {
    uint32_t v = 0;
    for (int i = 0; i < count; i++) {
        v = (v << 1) | sps_br_read_bit(br);
    }
    return v;
}

static inline uint32_t sps_br_read_ue(sps_bit_reader_t *br) {
    int zeros = 0;
    while (br->ok && sps_br_read_bit(br) == 0) {
        zeros++;
        if (zeros > 31) {
            br->ok = 0;
            return 0;
        }
    }
    if (!br->ok) return 0;
    uint32_t suffix = zeros ? sps_br_read_bits(br, zeros) : 0;
    return ((uint32_t)1 << zeros) - 1 + suffix;
}

static inline int32_t sps_br_read_se(sps_bit_reader_t *br) {
    uint32_t code_num = sps_br_read_ue(br);
    int32_t v = (int32_t)((code_num + 1) >> 1);
    return (code_num & 1) ? v : -v;
}

static inline void sps_bw_init(sps_bit_writer_t *bw,
                               uint8_t *data,
                               size_t cap) {
    bw->data = data;
    bw->cap = cap;
    bw->bitpos = 0;
    bw->ok = 1;
    if (cap > 0) memset(data, 0, cap);
}

static inline void sps_bw_write_bit(sps_bit_writer_t *bw, uint32_t bit) {
    if (!bw->ok || (bw->bitpos >> 3) >= bw->cap) {
        bw->ok = 0;
        return;
    }
    size_t byte_pos = bw->bitpos >> 3;
    int shift = 7 - (int)(bw->bitpos & 7);
    if (bit & 1) bw->data[byte_pos] |= (uint8_t)(1u << shift);
    bw->bitpos++;
}

static inline void sps_bw_write_bits(sps_bit_writer_t *bw,
                                     uint32_t value,
                                     int count) {
    for (int i = count - 1; i >= 0; i--) {
        sps_bw_write_bit(bw, (value >> i) & 1);
    }
}

static inline void sps_bw_write_ue(sps_bit_writer_t *bw, uint32_t value) {
    uint32_t code_num = value + 1;
    int bits = 0;
    for (uint32_t tmp = code_num; tmp; tmp >>= 1) bits++;
    int leading_zero_bits = bits - 1;
    for (int i = 0; i < leading_zero_bits; i++) sps_bw_write_bit(bw, 0);
    sps_bw_write_bits(bw, code_num, bits);
}

static inline void sps_bw_write_se(sps_bit_writer_t *bw, int32_t value) {
    uint32_t code_num = value <= 0
        ? (uint32_t)(-value * 2)
        : (uint32_t)(value * 2 - 1);
    sps_bw_write_ue(bw, code_num);
}

static inline size_t sps_bw_bytes(const sps_bit_writer_t *bw) {
    return (bw->bitpos + 7) >> 3;
}

static inline size_t sps_ebsp_to_rbsp(const uint8_t *ebsp,
                                      size_t ebsp_len,
                                      uint8_t *rbsp,
                                      size_t rbsp_cap) {
    size_t out = 0;
    int zero_count = 0;
    for (size_t i = 0; i < ebsp_len; i++) {
        uint8_t b = ebsp[i];
        if (zero_count >= 2 && b == 0x03) {
            zero_count = 0;
            continue;
        }
        if (out >= rbsp_cap) return 0;
        rbsp[out++] = b;
        zero_count = (b == 0) ? zero_count + 1 : 0;
    }
    return out;
}

static inline size_t sps_rbsp_to_ebsp(const uint8_t *rbsp,
                                      size_t rbsp_len,
                                      uint8_t *ebsp,
                                      size_t ebsp_cap) {
    size_t out = 0;
    int zero_count = 0;
    for (size_t i = 0; i < rbsp_len; i++) {
        uint8_t b = rbsp[i];
        if (zero_count >= 2 && b <= 0x03) {
            if (out >= ebsp_cap) return 0;
            ebsp[out++] = 0x03;
            zero_count = 0;
        }
        if (out >= ebsp_cap) return 0;
        ebsp[out++] = b;
        zero_count = (b == 0) ? zero_count + 1 : 0;
    }
    return out;
}

static inline int sps_find_start_code(const uint8_t *data,
                                      size_t len,
                                      size_t from,
                                      size_t *start,
                                      size_t *nal_start) {
    for (size_t i = from; i + 3 < len; i++) {
        if (data[i] != 0 || data[i + 1] != 0) continue;
        if (data[i + 2] == 1) {
            *start = i;
            *nal_start = i + 3;
            return 1;
        }
        if (i + 4 <= len && data[i + 2] == 0 && data[i + 3] == 1) {
            *start = i;
            *nal_start = i + 4;
            return 1;
        }
    }
    return 0;
}

static inline void sps_patch_constraints(uint8_t *nal, size_t len) {
    size_t offset = 0;
    if (len >= 5 && nal[0] == 0 && nal[1] == 0 && nal[2] == 0 && nal[3] == 1) {
        offset = 4;
    } else if (len >= 4 && nal[0] == 0 && nal[1] == 0 && nal[2] == 1) {
        offset = 3;
    }
    if (offset + 3 >= len) return;
    if ((nal[offset] & 0x1F) != 7) return;
    nal[offset + 2] |= 0x40;
    nal[offset + 2] |= 0x08;
    nal[offset + 2] |= 0x04;
}

static inline void sps_write_low_latency_vui(sps_bit_writer_t *bw) {
    sps_bw_write_bit(bw, 0);   /* aspect_ratio_info_present_flag */
    sps_bw_write_bit(bw, 0);   /* overscan_info_present_flag */
    sps_bw_write_bit(bw, 0);   /* video_signal_type_present_flag */
    sps_bw_write_bit(bw, 0);   /* chroma_loc_info_present_flag */
    sps_bw_write_bit(bw, 0);   /* timing_info_present_flag */
    sps_bw_write_bit(bw, 0);   /* nal_hrd_parameters_present_flag */
    sps_bw_write_bit(bw, 0);   /* vcl_hrd_parameters_present_flag */
    sps_bw_write_bit(bw, 0);   /* pic_struct_present_flag */
    sps_bw_write_bit(bw, 1);   /* bitstream_restriction_flag */
    sps_bw_write_bit(bw, 1);   /* motion_vectors_over_pic_boundaries_flag */
    sps_bw_write_ue(bw, 2);    /* max_bytes_per_pic_denom */
    sps_bw_write_ue(bw, 1);    /* max_bits_per_mb_denom */
    sps_bw_write_ue(bw, 16);   /* log2_max_mv_length_horizontal */
    sps_bw_write_ue(bw, 16);   /* log2_max_mv_length_vertical */
    sps_bw_write_ue(bw, 0);    /* num_reorder_frames */
    sps_bw_write_ue(bw, 1);    /* max_dec_frame_buffering */
}

static inline int sps_rewrite_baseline_rbsp(const uint8_t *rbsp,
                                            size_t rbsp_len,
                                            uint8_t *out,
                                            size_t out_cap,
                                            size_t *out_len) {
    sps_bit_reader_t br;
    sps_bit_writer_t bw;
    sps_br_init(&br, rbsp, rbsp_len);
    sps_bw_init(&bw, out, out_cap);

    uint32_t profile_idc = sps_br_read_bits(&br, 8);
    uint32_t constraints = sps_br_read_bits(&br, 8);
    uint32_t level_idc = sps_br_read_bits(&br, 8);
    if (!br.ok) return 0;

    /* Keep this narrow until we need to support a non-Baseline encoder path. */
    if (profile_idc != 66) return 0;

    sps_bw_write_bits(&bw, profile_idc, 8);
    sps_bw_write_bits(&bw, constraints | 0x4C, 8);
    sps_bw_write_bits(&bw, level_idc, 8);

    sps_bw_write_ue(&bw, sps_br_read_ue(&br)); /* seq_parameter_set_id */
    sps_bw_write_ue(&bw, sps_br_read_ue(&br)); /* log2_max_frame_num_minus4 */

    uint32_t pic_order_cnt_type = sps_br_read_ue(&br);
    sps_bw_write_ue(&bw, pic_order_cnt_type);
    if (pic_order_cnt_type == 0) {
        sps_bw_write_ue(&bw, sps_br_read_ue(&br));
    } else if (pic_order_cnt_type == 1) {
        sps_bw_write_bit(&bw, sps_br_read_bit(&br));
        sps_bw_write_se(&bw, sps_br_read_se(&br));
        sps_bw_write_se(&bw, sps_br_read_se(&br));
        uint32_t cycle = sps_br_read_ue(&br);
        sps_bw_write_ue(&bw, cycle);
        for (uint32_t i = 0; i < cycle && br.ok; i++) {
            sps_bw_write_se(&bw, sps_br_read_se(&br));
        }
    } else {
        return 0;
    }

    (void)sps_br_read_ue(&br);          /* original num_ref_frames */
    sps_bw_write_ue(&bw, 1);            /* num_ref_frames */
    sps_bw_write_bit(&bw, sps_br_read_bit(&br)); /* gaps flag */
    sps_bw_write_ue(&bw, sps_br_read_ue(&br));   /* width */
    sps_bw_write_ue(&bw, sps_br_read_ue(&br));   /* height */

    uint32_t frame_mbs_only = sps_br_read_bit(&br);
    sps_bw_write_bit(&bw, frame_mbs_only);
    if (!frame_mbs_only) {
        sps_bw_write_bit(&bw, sps_br_read_bit(&br));
    }
    sps_bw_write_bit(&bw, sps_br_read_bit(&br)); /* direct_8x8_inference */

    uint32_t crop = sps_br_read_bit(&br);
    sps_bw_write_bit(&bw, crop);
    if (crop) {
        sps_bw_write_ue(&bw, sps_br_read_ue(&br));
        sps_bw_write_ue(&bw, sps_br_read_ue(&br));
        sps_bw_write_ue(&bw, sps_br_read_ue(&br));
        sps_bw_write_ue(&bw, sps_br_read_ue(&br));
    }

    (void)sps_br_read_bit(&br);         /* original vui_parameters_present_flag */
    sps_bw_write_bit(&bw, 1);
    sps_write_low_latency_vui(&bw);

    sps_bw_write_bit(&bw, 1);           /* rbsp_stop_one_bit */
    while (bw.bitpos & 7) sps_bw_write_bit(&bw, 0);

    if (!br.ok || !bw.ok) return 0;
    *out_len = sps_bw_bytes(&bw);
    return *out_len > 0;
}

static inline int sps_patch_single_nal(uint8_t *nal,
                                       size_t nal_len,
                                       size_t cap,
                                       size_t *new_nal_len) {
    if (nal_len < 4 || (nal[0] & 0x1F) != 7) return 0;

    uint8_t rbsp[4096];
    uint8_t patched_rbsp[4096];
    uint8_t patched_ebsp[4096];

    size_t rbsp_len = sps_ebsp_to_rbsp(nal + 1, nal_len - 1,
                                       rbsp, sizeof(rbsp));
    if (rbsp_len == 0) {
        sps_patch_constraints(nal, nal_len);
        return 0;
    }

    size_t patched_rbsp_len = 0;
    if (!sps_rewrite_baseline_rbsp(rbsp, rbsp_len,
                                   patched_rbsp, sizeof(patched_rbsp),
                                   &patched_rbsp_len)) {
        sps_patch_constraints(nal, nal_len);
        return 0;
    }

    size_t patched_ebsp_len = sps_rbsp_to_ebsp(patched_rbsp, patched_rbsp_len,
                                               patched_ebsp,
                                               sizeof(patched_ebsp));
    if (patched_ebsp_len == 0 || 1 + patched_ebsp_len > cap) {
        sps_patch_constraints(nal, nal_len);
        return 0;
    }

    nal[0] = (uint8_t)((nal[0] & 0xE0) | 7);
    memcpy(nal + 1, patched_ebsp, patched_ebsp_len);
    *new_nal_len = 1 + patched_ebsp_len;
    return 1;
}

static inline int sps_patch_h264_low_latency(uint8_t *data,
                                             size_t *len,
                                             size_t cap) {
    if (!data || !len || *len == 0 || cap < *len) return 0;

    int patched = 0;
    size_t start = 0;
    size_t nal_start = 0;
    size_t search = 0;

    if (!sps_find_start_code(data, *len, 0, &start, &nal_start)) {
        size_t new_nal_len = *len;
        if (sps_patch_single_nal(data, *len, cap, &new_nal_len)) {
            *len = new_nal_len;
            return 1;
        }
        return 0;
    }

    search = start;
    while (sps_find_start_code(data, *len, search, &start, &nal_start)) {
        size_t next_start = *len;
        size_t next_nal = 0;
        if (sps_find_start_code(data, *len, nal_start, &next_start, &next_nal)) {
            (void)next_nal;
        }

        size_t nal_len = next_start > nal_start ? next_start - nal_start : 0;
        if (nal_len > 0 && (data[nal_start] & 0x1F) == 7) {
            uint8_t old_header = data[nal_start];
            size_t new_nal_len = nal_len;
            uint8_t patched_nal[4096];
            if (nal_len <= sizeof(patched_nal)) {
                memcpy(patched_nal, data + nal_start, nal_len);
                if (sps_patch_single_nal(patched_nal, nal_len,
                                         sizeof(patched_nal),
                                         &new_nal_len)) {
                    size_t new_total = *len - nal_len + new_nal_len;
                    if (new_total <= cap) {
                        memmove(data + nal_start + new_nal_len,
                                data + next_start,
                                *len - next_start);
                        patched_nal[0] = old_header;
                        memcpy(data + nal_start, patched_nal, new_nal_len);
                        *len = new_total;
                        patched = 1;
                        search = nal_start + new_nal_len;
                        continue;
                    }
                }
            }
            sps_patch_constraints(data + nal_start, nal_len);
        }

        search = next_start;
    }

    return patched;
}

#ifdef __cplusplus
}
#endif

#endif /* DROIDSCREEN_SPS_PATCH_H */
