// LED bench firmware for stage 2 of the basement tests (docs/protocol-design.md, section 8): the
// same cues as the sync bench, but drawn on a strip of pixels by a Conductor, so you can see
// whether the boards agree.
//
// Built twice: led_bench_leader and led_bench_follower. Each drives kPixels pixels on GP2. With no
// leader in sight a board plays its local playlist (Starlight); once it joins, the leader picks:
// Starlight for 20 s, then warm white for 5 s, then four TestFlash cues a second apart (a sharp
// 100 ms flash in red, green, blue and white), and round again. The flashes are the ones to watch
// side by side or film in slow motion.

#include <cstdio>
#include <cstring>

#include "Canvas.h"
#include "Starlight.h"
#include "Strip.h"
#include "credentials.h"
#include "pico/stdlib.h"
#include "picocomm/conductor.h"
#include "picocomm/pico/node.h"

using picocomm::Conductor;
using picocomm::Cue;
using picocomm::CueKind;
using picocomm::EffectId;
using picocomm::pico::Node;
using picocomm::pico::NodeConfig;
using picocomm::pico::Role;

namespace {

constexpr uint kStripPin = 2;
constexpr int kPixels = 100;
constexpr uint8_t kBrightness = 48;
constexpr uint16_t kFwVersion = 1;
constexpr uint64_t kReportIntervalUs = 10000000;

// SolidColor: params [0..2] r, g, b.
class Solid : public Animation {
   public:
    Solid(Canvas *c, RGB color) : Animation(c, nullptr, 0), color(color) {}
    bool step() override {
        canvas->fill(color);
        return true;
    }

   private:
    RGB color;
};

// TestFlash: params [0..2] r, g, b; on for the first 100 ms after the cue, then off.
class Flash : public Animation {
   public:
    Flash(Canvas *c, RGB color) : Animation(c, nullptr, 0), color(color) {}
    bool step() override {
        canvas->fill(elapsedUs() < 100000 ? color : RGB(0, 0, 0));
        return true;
    }

   private:
    RGB color;
};

RGB colorOf(const Cue &cue) { return RGB(cue.params[0], cue.params[1], cue.params[2]); }

Animation *makeSolid(Canvas *c, const Cue &cue) { return new Solid(c, colorOf(cue)); }
Animation *makeFlash(Canvas *c, const Cue &cue) { return new Flash(c, colorOf(cue)); }
Animation *makeStarlight(Canvas *c, const Cue &) { return new Starlight(c); }

// The leader's show: what to cue next, and how long until the one after.
struct Scene {
    EffectId effect;
    uint8_t r, g, b;
    uint32_t holdMs;
};
const Scene kShow[] = {
    {EffectId::Starlight, 0, 0, 0, 20000},   {EffectId::SolidColor, 255, 180, 100, 5000},
    {EffectId::TestFlash, 255, 0, 0, 1000},  {EffectId::TestFlash, 0, 255, 0, 1000},
    {EffectId::TestFlash, 0, 0, 255, 1000},  {EffectId::TestFlash, 255, 255, 255, 1000},
};
constexpr int kShowLength = sizeof(kShow) / sizeof(kShow[0]);

}  // namespace

int main() {
    stdio_init_all();
    bench::getCredentials();
    const bool isLeader = PICOCOMM_BENCH_LEADER;
    printf("\nPicoComm LED bench: %s\n", isLeader ? "LEADER" : "follower");

    static const uint16_t effects[] = {(uint16_t)EffectId::None, (uint16_t)EffectId::SolidColor,
                                       (uint16_t)EffectId::Starlight, (uint16_t)EffectId::TestFlash};
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

    // Only now: the radio has claimed its PIO state machine, so the renderer can't take it.
    static Canvas canvas(kPixels);
    static Strip strip(kStripPin, kPixels);
    canvas.add(&strip);
    canvas.setup();
    canvas.getRenderer()->setBrightness(kBrightness);

    static Conductor conductor(&canvas, node);
    conductor.addEffect(EffectId::SolidColor, makeSolid);
    conductor.addEffect(EffectId::TestFlash, makeFlash);
    conductor.addEffect(EffectId::Starlight, makeStarlight);
    conductor.addLocal(EffectId::Starlight, 60000);
    conductor.init();

    int scene = 0;
    uint64_t nextScene = 0;
    uint64_t nextReport = 0;
    uint16_t lastCueId = 0;
    while (true) {
        conductor.step();

        uint64_t show = conductor.getFrameTime();
        if (conductor.currentCueId() != lastCueId) {
            lastCueId = conductor.currentCueId();
            printf("cue %u: effect 0x%04x\n", lastCueId, conductor.currentEffect());
        }

        if (isLeader && show >= nextScene) {
            const Scene &s = kShow[scene];
            scene = (scene + 1) % kShowLength;
            nextScene = show + (uint64_t)s.holdMs * 1000;
            Cue cue;
            cue.kind = CueKind::Effect;
            cue.effect = (uint16_t)s.effect;
            cue.params[0] = s.r;
            cue.params[1] = s.g;
            cue.params[2] = s.b;
            if (!node.cue(picocomm::kAllGroups, cue)) {
                printf("director: cue queue full\n");
            }
        }

        if (show >= nextReport || show + kReportIntervalUs < nextReport) {  // or show time went back
            nextReport = show + kReportIntervalUs;
            printf("[%7.1f s] %s, rssi %d dBm, %s, %u missed frames\n", show / 1e6,
                   picocomm::pico::toString(node.state()), node.rssi(),
                   conductor.playingLocal() ? "local playlist" : "following cues",
                   conductor.getMissedFrames());
        }
    }
}
