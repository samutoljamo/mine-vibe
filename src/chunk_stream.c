#include "chunk_stream.h"

int chunk_stream_in_range(int32_t center_cx, int32_t center_cz,
                          int32_t cx, int32_t cz, int render_dist) {
    if (render_dist < 0) return 0;
    long dx = (long)cx - center_cx;
    long dz = (long)cz - center_cz;
    return dx*dx + dz*dz <= (long)render_dist * render_dist;
}

static int coord_in_set(const ChunkCoord* set, size_t n, int32_t cx, int32_t cz) {
    for (size_t i = 0; i < n; i++)
        if (set[i].cx == cx && set[i].cz == cz) return 1;
    return 0;
}

static long dist_sq(int32_t ccx, int32_t ccz, int32_t cx, int32_t cz) {
    long dx = (long)cx - ccx;
    long dz = (long)cz - ccz;
    return dx*dx + dz*dz;
}

void chunk_stream_diff(int32_t center_cx, int32_t center_cz, int render_dist,
                       const ChunkCoord* sent, size_t sent_count,
                       ChunkCoord* to_send, size_t send_cap, size_t* out_send,
                       int send_budget,
                       ChunkCoord* to_unload, size_t unload_cap,
                       size_t* out_unload) {
    size_t ns = 0, nu = 0;

    /* ---- Unload pass: sent coords now out of range ---- */
    for (size_t i = 0; i < sent_count; i++) {
        if (!chunk_stream_in_range(center_cx, center_cz,
                                   sent[i].cx, sent[i].cz, render_dist)) {
            if (nu < unload_cap) to_unload[nu] = sent[i];
            nu++;   /* count even past cap so caller can detect truncation */
        }
    }
    if (nu > unload_cap) nu = unload_cap;
    if (out_unload) *out_unload = nu;

    /* ---- Send pass: in-range coords not yet sent, nearest-first ----
     * Effective cap = min(send_cap, budget) when budget > 0. We keep the
     * to_send array sorted by squared distance via a bounded insertion sort, so
     * no scratch buffer for the whole disc is needed and the result is always
     * the `eff_cap` nearest pending chunks. */
    size_t eff_cap = send_cap;
    if (send_budget > 0 && (size_t)send_budget < eff_cap)
        eff_cap = (size_t)send_budget;

    if (render_dist >= 0 && eff_cap > 0) {
        long rd_sq = (long)render_dist * render_dist;
        for (int dx = -render_dist; dx <= render_dist; dx++) {
            for (int dz = -render_dist; dz <= render_dist; dz++) {
                long d = (long)dx*dx + (long)dz*dz;
                if (d > rd_sq) continue;

                int32_t cx = center_cx + dx;
                int32_t cz = center_cz + dz;
                if (coord_in_set(sent, sent_count, cx, cz)) continue;

                /* If full and not closer than the current farthest, skip. */
                if (ns == eff_cap) {
                    long worst = dist_sq(center_cx, center_cz,
                                         to_send[ns-1].cx, to_send[ns-1].cz);
                    if (d >= worst) continue;
                }

                /* Make room: shift strictly-farther entries right by one. When
                 * full, the farthest entry falls off the end. */
                size_t j = (ns < eff_cap) ? ns : eff_cap - 1;
                while (j > 0) {
                    long ed = dist_sq(center_cx, center_cz,
                                      to_send[j-1].cx, to_send[j-1].cz);
                    if (ed <= d) break;   /* stable: keep equal-distance order */
                    to_send[j] = to_send[j-1];
                    j--;
                }
                to_send[j].cx = cx;
                to_send[j].cz = cz;
                if (ns < eff_cap) ns++;
            }
        }
    }
    if (out_send) *out_send = ns;
}
