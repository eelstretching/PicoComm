// The judge: a plain Pico (or Pico 2; no WiFi needed) that measures how closely the bench boards
// agree on the time. Wire each board's GP22 through a 1 kOhm resistor to GP2, GP3, GP4, ... here,
// with all grounds joined (docs/protocol-design.md, section 8, has the table).
//
// The boards pulse once per second of show time. The judge spins on gpio_get_all() and
// timestamps every rising edge with time_us_64() (1 us resolution; edges on different pins in the
// same loop iteration get the same timestamp, so nothing queues behind anything). Edges within
// 200 ms of each other are one round. After each round it prints every board's edge relative to
// the earliest, and every 10 rounds a summary of the spread so far.
//
// The same wiring on the boards' GP21 measures cue alignment instead.

#include <cstdio>

#include "pico/stdlib.h"

namespace {

constexpr uint kFirstPin = 2;
constexpr int kInputs = 8;  // GP2 to GP9: boards A to H
constexpr uint64_t kRoundWindowUs = 200000;

struct Stats {
    uint32_t rounds = 0;
    uint32_t under100 = 0;
    uint32_t under500 = 0;
    uint32_t under1000 = 0;
    uint32_t over1000 = 0;
    uint64_t worst = 0;
    uint64_t total = 0;
};

}  // namespace

int main() {
    stdio_init_all();
    uint32_t mask = 0;
    for (int i = 0; i < kInputs; i++) {
        uint pin = kFirstPin + i;
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_IN);
        gpio_pull_down(pin);  // unconnected inputs stay low
        mask |= 1u << pin;
    }
    sleep_ms(1500);
    printf("\nPicoComm judge: boards A-H on GP%u-GP%u\n", kFirstPin, kFirstPin + kInputs - 1);

    Stats stats;
    uint32_t everSeen = 0;
    uint32_t prev = gpio_get_all() & mask;
    bool open = false;
    uint64_t roundStart = 0;
    uint32_t seen = 0;
    uint64_t edge[kInputs] = {};

    while (true) {
        uint32_t now = gpio_get_all() & mask;
        uint64_t t = time_us_64();
        uint32_t rising = now & ~prev;
        prev = now;
        if (rising) {
            if (!open) {
                open = true;
                roundStart = t;
                seen = 0;
            }
            for (int i = 0; i < kInputs; i++) {
                uint32_t bit = 1u << (kFirstPin + i);
                if ((rising & bit) && !(seen & bit)) {
                    edge[i] = t;
                    seen |= bit;
                }
            }
        }
        if (!open || t - roundStart < kRoundWindowUs) {
            continue;
        }

        // Round over: report it.
        open = false;
        everSeen |= seen;
        uint64_t first = UINT64_MAX, last = 0;
        int boards = 0;
        for (int i = 0; i < kInputs; i++) {
            if (seen & (1u << (kFirstPin + i))) {
                boards++;
                first = edge[i] < first ? edge[i] : first;
                last = edge[i] > last ? edge[i] : last;
            }
        }
        uint64_t spread = last - first;
        printf("spread %5llu us |", (unsigned long long)spread);
        for (int i = 0; i < kInputs; i++) {
            uint32_t bit = 1u << (kFirstPin + i);
            if (seen & bit) {
                printf("  %c +%llu", 'A' + i, (unsigned long long)(edge[i] - first));
            } else if (everSeen & bit) {
                printf("  %c --", 'A' + i);
            }
        }
        printf("\n");

        if (boards >= 2) {
            stats.rounds++;
            stats.total += spread;
            stats.worst = spread > stats.worst ? spread : stats.worst;
            if (spread < 100) {
                stats.under100++;
            } else if (spread < 500) {
                stats.under500++;
            } else if (spread < 1000) {
                stats.under1000++;
            } else {
                stats.over1000++;
            }
            if (stats.rounds % 10 == 0) {
                printf("-- %lu rounds: mean %llu us, worst %llu us; <100 us %lu, <500 %lu, "
                       "<1000 %lu, >=1000 %lu\n",
                       (unsigned long)stats.rounds, (unsigned long long)(stats.total / stats.rounds),
                       (unsigned long long)stats.worst, (unsigned long)stats.under100,
                       (unsigned long)stats.under500, (unsigned long)stats.under1000,
                       (unsigned long)stats.over1000);
            }
        }
    }
}
