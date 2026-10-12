// Bench firmware for stages 1 and 2 of the basement tests (docs/protocol-design.md, section 8).
//
// Built twice: sync_bench_leader and sync_bench_follower. Every board:
// - pulses GP22 high for 1 ms at every whole second of show time, so a logic analyzer or the
//   judge Pico can measure how closely the boards agree on the time;
// - pulses GP21 high for 1 ms when a cue takes effect, so cue alignment can be measured the same
//   way;
// - prints protocol events and a status line every 10 s over USB serial.
//
// The leader also cues a test scene to everyone every 5 s (TestFlash in a rotating color).
// There are no LEDs yet: this is about the clocks and the cues.

#include <cstdio>
#include <cstring>
#include <initializer_list>

#include "pico/stdlib.h"

#include "credentials.h"
#include "picocomm/pico/node.h"

using picocomm::Cue;
using picocomm::CueKind;
using picocomm::EffectId;
using picocomm::pico::Node;
using picocomm::pico::NodeConfig;
using picocomm::pico::Role;

namespace {

constexpr uint kSecondPin = 22;
constexpr uint kCuePin = 21;
constexpr uint64_t kPulseUs = 1000;
constexpr uint64_t kSceneIntervalUs = 5000000;
constexpr uint64_t kReportIntervalUs = 10000000;
constexpr uint16_t kFwVersion = 1;

// Cues waiting for their show time.
constexpr int kMaxWaiting = 8;
Cue waiting[kMaxWaiting];
int waitingCount = 0;

}  // namespace

int main() {
    stdio_init_all();
    for (uint pin : {kSecondPin, kCuePin}) {
        gpio_init(pin);
        gpio_set_dir(pin, GPIO_OUT);
        gpio_put(pin, false);
    }

    bench::getCredentials();
    const bool isLeader = PICOCOMM_BENCH_LEADER;
    printf("\nPicoComm sync bench: %s\n", isLeader ? "LEADER" : "follower");

    static const uint16_t effects[] = {(uint16_t)EffectId::None, (uint16_t)EffectId::SolidColor,
                                       (uint16_t)EffectId::TestFlash};
    NodeConfig cfg;
    cfg.role = isLeader ? Role::Leader : Role::Follower;
    cfg.propType = picocomm::PropType::Bench;
    cfg.fwVersion = kFwVersion;
    cfg.ssid = bench::ssid;
    cfg.password = bench::password;
    cfg.effects = effects;
    cfg.effectCount = sizeof(effects) / sizeof(effects[0]);
    static Node node(cfg);
    node.start();
    if (!node.waitForRadio()) {
        printf("Radio failed to start.\n");
    }
    // A prop would call canvas.setup() here, after the radio has its PIO state machine.

    uint64_t nextScene = 0;
    uint64_t nextReport = 0;
    uint64_t cuePulseUntil = 0;
    uint32_t cuesApplied = 0;
    uint16_t lastCueId = 0, lastEffect = 0;
    uint8_t sceneColor = 0;
    bool wasLocked = false;

    while (true) {
        uint64_t show;
        bool locked = node.showTime(show);
        if (locked != wasLocked) {
            printf("show clock %s\n", locked ? "locked" : "lost");
            wasLocked = locked;
        }
        if (!locked) {
            gpio_put(kSecondPin, false);
            gpio_put(kCuePin, false);
            continue;
        }

        gpio_put(kSecondPin, show % 1000000 < kPulseUs);

        Cue c;
        while (node.popCue(c)) {
            if (waitingCount < kMaxWaiting) {
                waiting[waitingCount++] = c;
            } else {
                printf("too many cues waiting, dropped cue %u\n", c.cueId);
            }
        }
        for (int i = 0; i < waitingCount;) {
            if (show >= waiting[i].atUs) {
                const Cue &w = waiting[i];
                long lateUs = (long)(show - w.atUs);
                cuePulseUntil = show + kPulseUs;
                cuesApplied++;
                lastCueId = w.cueId;
                if (w.kind == CueKind::Effect) {
                    lastEffect = w.effect;
                }
                printf("cue %u: kind %u effect 0x%04x rgb %u,%u,%u at %llu (%ld us after its time)\n",
                       w.cueId, (unsigned)w.kind, w.effect, w.params[0], w.params[1], w.params[2],
                       (unsigned long long)w.atUs, lateUs);
                waiting[i] = waiting[--waitingCount];
            } else {
                i++;
            }
        }
        gpio_put(kCuePin, show < cuePulseUntil);

        if (isLeader && show >= nextScene) {
            nextScene = show + kSceneIntervalUs;
            static const uint8_t colors[][3] = {{255, 0, 0}, {0, 255, 0}, {0, 0, 255}, {255, 255, 255}};
            Cue scene;
            scene.kind = CueKind::Effect;
            scene.effect = (uint16_t)EffectId::TestFlash;
            memcpy(scene.params, colors[sceneColor], 3);
            sceneColor = (sceneColor + 1) % 4;
            if (!node.cue(picocomm::kAllGroups, scene)) {
                printf("director: cue queue full\n");
            }
        }

        if (show >= nextReport) {
            nextReport = show + kReportIntervalUs;
            node.reportFrame(0, 0, lastEffect, lastCueId);
            printf("[%7.1f s] %s, rssi %d dBm, %lu cues applied, %lu dropped\n", show / 1e6,
                   picocomm::pico::toString(node.state()), node.rssi(), (unsigned long)cuesApplied,
                   (unsigned long)node.cuesDropped());
        }
    }
}
