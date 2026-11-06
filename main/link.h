// Two-badge pairing and heartbeat over ESP-NOW. Pure logic, no ESP-IDF calls, so tools/test_link.c runs it on a PC.
// Times are in ms. Protocol: NOTES.md "Pong over ESP-NOW".
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define LINK_WINDOW 500   // both A presses must be at most this far apart
#define LINK_DECIDE 600   // after my press: the candidate list is closed (window + air time)
#define LINK_GIVEUP 1500  // after my press: no mutual choice yet, back to free
#define LINK_TIMEOUT 1000 // paired: no packet from the peer for this long, back to free
#define LINK_PAIR_MS 50   // seeking: broadcast period
#define LINK_GAME_MS 100  // paired: unicast period (the game packet is the heartbeat)

enum { L_FREE, L_SEEK, L_PAIRED };
enum { P_PAIR = 1, P_GAME = 2 };

typedef struct __attribute__((packed)) {
    uint8_t tick;          // host's game step, never 0 once running
    int8_t bx, by, dx, dy; // ball cell and direction
    uint8_t py[2];         // paddle top row: [0] host (left), [1] guest (right)
    uint8_t score[2];
} game_t;

typedef struct __attribute__((packed)) {
    char magic[4];   // "PONG"
    uint8_t type;    // P_PAIR (broadcast) or P_GAME (unicast)
    uint16_t age;    // P_PAIR: ms since the sender's A press
    uint8_t peer[6]; // the sender's choice (P_PAIR, zero while undecided) or its peer (P_GAME)
    uint8_t seen;    // guest: last tick it has shown
    game_t g;        // host: the game; guest: only g.py[1] counts
} pkt_t;

typedef struct {
    int state;
    uint8_t me[6], peer[6]; // seeking: peer = my choice, zero until LINK_DECIDE
    int64_t press, last_rx;
    struct { uint8_t mac[6]; int64_t press; } heard[4]; // recent P_PAIR senders and their press times
} link_t;

static const uint8_t link_zero[6], link_bcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

static void link_press(link_t *l, int64_t now)
{
    if (l->state == L_PAIRED) return; // only a reboot ends a pairing
    l->state = L_SEEK;
    l->press = now;
    memset(l->peer, 0, 6);
}

static void link_rx(link_t *l, const uint8_t *src, const pkt_t *p, int64_t now)
{
    if (memcmp(p->magic, "PONG", 4)) return;
    if (p->type == P_PAIR && l->state != L_PAIRED) { // remember when the sender pressed A
        int k = 0;
        for (int i = 0; i < 4; i++) {
            if (!memcmp(l->heard[i].mac, src, 6)) { k = i; break; }
            if (l->heard[i].press < l->heard[k].press) k = i; // else overwrite the oldest
        }
        memcpy(l->heard[k].mac, src, 6);
        l->heard[k].press = now - p->age;
    }
    // Bind only on a mutual choice: I picked src, and src says it picked me.
    bool mutual = !memcmp(src, l->peer, 6) && !memcmp(p->peer, l->me, 6);
    if (l->state == L_SEEK && memcmp(l->peer, link_zero, 6) && mutual) l->state = L_PAIRED;
    if (l->state == L_PAIRED && mutual) l->last_rx = now;
}

static void link_tick(link_t *l, int64_t now)
{
    if (l->state == L_SEEK && now - l->press >= LINK_GIVEUP) l->state = L_FREE;
    if (l->state == L_SEEK && now - l->press >= LINK_DECIDE && !memcmp(l->peer, link_zero, 6)) {
        int n = 0, k = 0;
        for (int i = 0; i < 4; i++)
            if (memcmp(l->heard[i].mac, link_zero, 6) && llabs(l->heard[i].press - l->press) <= LINK_WINDOW) n++, k = i;
        if (n == 1) memcpy(l->peer, l->heard[k].mac, 6);
        else l->state = L_FREE; // nobody, or more than one badge in the window (two pairs at once): press again
    }
    if (l->state == L_PAIRED && now - l->last_rx > LINK_TIMEOUT) l->state = L_FREE;
}

// Header of the next packet; returns where it goes (NULL: nothing to send in this state).
static const uint8_t *link_pkt(const link_t *l, int64_t now, pkt_t *p)
{
    if (l->state == L_FREE) return NULL;
    memcpy(p->magic, "PONG", 4);
    p->type = l->state == L_SEEK ? P_PAIR : P_GAME;
    p->age = l->state == L_SEEK ? now - l->press : 0;
    memcpy(p->peer, l->peer, 6);
    return l->state == L_SEEK ? link_bcast : l->peer;
}
