/*
 * Copyright (c) 2019 Fastly, Janardhan Iyengar
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 */
#include "quicly/cc.h"
#include "quicly.h"

#define QUICLY_MIN_CWND 2
#define QUICLY_RENO_BETA 0.7
#define QUICLY_RENO_LOSS_THRESHOLD 2

/* TODO: Avoid increase if sender was application limited. */
static void reno_on_acked(quicly_cc_t *cc, const quicly_loss_t *loss, uint32_t bytes, uint64_t largest_acked, uint32_t inflight,
                          int64_t now, uint32_t max_udp_payload_size)
{
    assert(inflight >= bytes);
    /* Do not increase congestion window while in recovery and if number of
     * losses in this episode is greater than the threshold. */
    if (largest_acked < cc->recovery_end &&
        cc->state.reno.num_lost_in_episode >= QUICLY_RENO_LOSS_THRESHOLD)
        return;

    /* Slow start. */
    if (cc->cwnd < cc->ssthresh) {
        cc->cwnd += bytes;
        if (cc->cwnd_maximum < cc->cwnd)
            cc->cwnd_maximum = cc->cwnd;
        return;
    }
    /* Congestion avoidance. */
    cc->state.reno.stash += bytes;
    if (cc->state.reno.stash < cc->cwnd)
        return;
    /* Increase congestion window by 1 MSS per congestion window acked. */
    uint32_t count = cc->state.reno.stash / cc->cwnd;
    cc->state.reno.stash -= count * cc->cwnd;
    cc->cwnd += count * max_udp_payload_size;
    if (cc->cwnd_maximum < cc->cwnd)
        cc->cwnd_maximum = cc->cwnd;
}

static void reno_on_lost(quicly_cc_t *cc, const quicly_loss_t *loss, uint32_t bytes, uint64_t lost_pn, uint64_t next_pn,
                         int64_t now, uint32_t max_udp_payload_size)
{
    /* Set up state if potentially new recovery episode. */
    if (lost_pn >= cc->recovery_end) {
        cc->recovery_end = next_pn;
        cc->state.reno.num_lost_in_episode = 0;
    }

    cc->state.reno.num_lost_in_episode++;

    /* Nothing to do if number of losses in this recovery episode is below a
     * threshold. Doing so builds some tolerance for loss, by only responding
     * with a congestion action if the number of losses in a window is greater
     * than the threshold. Similarly, there should be only one reduction in a
     * window, so nothing to do if number of losses is greater than the
     * threshold.
     */
    if (cc->state.reno.num_lost_in_episode != QUICLY_RENO_LOSS_THRESHOLD)
        return;

    ++cc->num_loss_episodes;
    if (cc->cwnd_exiting_slow_start == 0)
        cc->cwnd_exiting_slow_start = cc->cwnd;

    /* Reduce congestion window. */
    cc->cwnd *= QUICLY_RENO_BETA;
    if (cc->cwnd < QUICLY_MIN_CWND * max_udp_payload_size)
        cc->cwnd = QUICLY_MIN_CWND * max_udp_payload_size;
    cc->ssthresh = cc->cwnd;

    if (cc->cwnd_minimum > cc->cwnd)
        cc->cwnd_minimum = cc->cwnd;
}

static void reno_on_persistent_congestion(quicly_cc_t *cc, const quicly_loss_t *loss, int64_t now)
{
    /* TODO */
}

static void reno_on_sent(quicly_cc_t *cc, const quicly_loss_t *loss, uint32_t bytes, int64_t now)
{
    /* Unused */
}

static const struct st_quicly_cc_impl_t reno_impl = {CC_RENO_MODIFIED, reno_on_acked, reno_on_lost, reno_on_persistent_congestion,
                                                     reno_on_sent};

static void reno_init(quicly_init_cc_t *self, quicly_cc_t *cc, uint32_t initcwnd, int64_t now)
{
    memset(cc, 0, sizeof(quicly_cc_t));
    cc->impl = &reno_impl;
    cc->cwnd = cc->cwnd_initial = cc->cwnd_maximum = initcwnd;
    cc->ssthresh = cc->cwnd_minimum = UINT32_MAX;
}

quicly_init_cc_t quicly_cc_reno_init = {reno_init};

uint32_t quicly_cc_calc_initial_cwnd(uint32_t max_packets, uint16_t max_udp_payload_size)
{
    static const uint32_t mtu_max = 1472;

    /* apply filters to the two arguments */
    if (max_packets < QUICLY_MIN_CWND)
        max_packets = QUICLY_MIN_CWND;
    if (max_udp_payload_size > mtu_max)
        max_udp_payload_size = mtu_max;

    return max_packets * max_udp_payload_size;
}
