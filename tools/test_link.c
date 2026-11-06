// Host-side check of the pairing/heartbeat state machine in main/link.h.
// cc -I main tools/test_link.c -o /tmp/test_link && /tmp/test_link
// Simulates N badges on one radio channel: 1 ms steps, 3 ms air time, 20% packet loss, a badge stops sending
// when it "reboots".
#include <assert.h>
#include <stdio.h>
#include "link.h"

#define N 4
static link_t b[N];
static int64_t next_tx[N], press_at[N], dead_at[N];
static struct { int64_t at; int from; uint8_t to[6]; pkt_t p; } air[4096];
static int nair;

static void reset(void)
{
    memset(b, 0, sizeof b);
    nair = 0;
    for (int i = 0; i < N; i++) {
        b[i].me[0] = 0x10, b[i].me[5] = i + 1;
        press_at[i] = dead_at[i] = -1;
        next_tx[i] = 0;
    }
}

static void run(int64_t until)
{
    static int64_t t;
    static unsigned rnd = 1;
    if (!nair) t = 1000;
    for (; t < until; t++) {
        for (int i = 0; i < N; i++) {
            if (dead_at[i] >= 0 && t >= dead_at[i]) continue;
            if (t == press_at[i]) link_press(&b[i], t);
            link_tick(&b[i], t);
            pkt_t p;
            const uint8_t *to = link_pkt(&b[i], t, &p);
            if (to && t >= next_tx[i]) {
                air[nair].at = t + 3, air[nair].from = i, air[nair].p = p;
                memcpy(air[nair++].to, to, 6);
                next_tx[i] = t + (b[i].state == L_SEEK ? LINK_PAIR_MS : LINK_GAME_MS);
            }
        }
        for (int k = 0; k < nair; k++) {
            if (air[k].at != t) continue;
            for (int i = 0; i < N; i++) {
                rnd = rnd * 1103515245 + 12345;
                bool lost = (rnd >> 16) % 5 == 0;
                bool addressed = !memcmp(air[k].to, link_bcast, 6) || !memcmp(air[k].to, b[i].me, 6);
                if (i != air[k].from && addressed && !lost && !(dead_at[i] >= 0 && t >= dead_at[i]))
                    link_rx(&b[i], b[air[k].from].me, &air[k].p, t);
            }
        }
    }
}

static bool paired(int i, int j) { return b[i].state == L_PAIRED && !memcmp(b[i].peer, b[j].me, 6); }

int main(void)
{
    // Two badges, presses 400 ms apart: they bind to each other.
    reset();
    press_at[0] = 1100, press_at[1] = 1500;
    run(3500);
    assert(paired(0, 1) && paired(1, 0));
    assert(b[2].state == L_FREE && b[3].state == L_FREE);

    // Paired stays paired: a new A press changes nothing.
    press_at[0] = 3600;
    run(6000);
    assert(paired(0, 1) && paired(1, 0));

    // Badge 1 reboots (goes silent): badge 0 is free after 1 s, not before.
    dead_at[1] = 6000;
    run(6000 + LINK_TIMEOUT - 50);
    assert(b[0].state == L_PAIRED);
    run(6000 + LINK_TIMEOUT + 50);
    assert(b[0].state == L_FREE);

    // Presses 700 ms apart: too far, nobody binds.
    reset();
    press_at[0] = 1100, press_at[1] = 1800;
    run(4000);
    for (int i = 0; i < N; i++) assert(b[i].state == L_FREE);

    // One badge alone: back to free.
    reset();
    press_at[2] = 1100;
    run(1100 + LINK_GIVEUP + 10);
    assert(b[2].state == L_FREE);

    // Two pairs press at the same moment: ambiguous, nobody binds (no cross-binding).
    reset();
    press_at[0] = 1100, press_at[1] = 1200, press_at[2] = 1150, press_at[3] = 1250;
    run(4000);
    for (int i = 0; i < N; i++) assert(b[i].state == L_FREE);

    // Two pairs 1 s apart: each binds to its own partner.
    reset();
    press_at[0] = 1100, press_at[1] = 1300, press_at[2] = 2100, press_at[3] = 2400;
    run(5000);
    assert(paired(0, 1) && paired(1, 0) && paired(2, 3) && paired(3, 2));

    puts("link: all checks passed");
    return 0;
}
