// Conductor on the desktop: the real Conductor and PicoLEDs (built for the PicoLEDs simulator),
// a pretend clock, and a fake CueSource standing in for the Node.

#include <cstdio>
#include <cstdlib>
#include <deque>

#include "Canvas.h"
#include "Strip.h"
#include "check.h"
#include "picocomm/conductor.h"
#include "sim_hooks.h"

using namespace picocomm;

// This Pico's own clock. Deliberately not on a frame boundary.
static uint64_t fakeClock = 5000003;

uint64_t time_us_64(void) { return fakeClock; }
void sleep_us(uint64_t us) { fakeClock += us; }
uint32_t get_rand_32(void) { return (uint32_t)rand(); }
void picoleds_sim_canvas_show(Canvas *) {}
Canvas *picoleds_sim_canvas_for(Renderer *) { return nullptr; }
void picoleds_sim_frame(int, int, const uint8_t *, uint64_t, bool) {}
uint16_t XY(uint8_t, uint8_t) { return 0; }

// Stands in for the Node: a show clock offset from this Pico's, and a queue of cues.
class FakeSource : public CueSource {
   public:
    bool locked = false;
    int64_t offsetUs = 0;
    std::deque<Cue> cues;
    uint16_t reportedEffect = 0xffff;
    uint16_t reportedCue = 0xffff;
    int reports = 0;

    uint64_t show() const { return fakeClock + offsetUs; }
    bool showTime(uint64_t &showUs) const override {
        if (!locked) {
            return false;
        }
        showUs = show();
        return true;
    }
    bool popCue(Cue &cue) override {
        if (cues.empty()) {
            return false;
        }
        cue = cues.front();
        cues.pop_front();
        return true;
    }
    void reportFrame(uint8_t, uint16_t, uint16_t effect, uint16_t cueId) override {
        reportedEffect = effect;
        reportedCue = cueId;
        reports++;
    }
    void reportFrameStats(const FrameStats &s) override {
        lastStats = s;
        statsReports++;
    }
    FrameStats lastStats;
    int statsReports = 0;
};

// An effect that fills the canvas with its color, and counts how many of its kind are alive.
static int alive = 0;
static int made = 0;

class Probe : public Animation {
   public:
    RGB color;
    int steps = 0;
    uint64_t firstFrame = 0;
    Probe(Canvas *c, RGB color) : Animation(c, nullptr, 0), color(color) {
        alive++;
        made++;
    }
    ~Probe() override { alive--; }
    void init() override { steps = 0; }
    bool step() override {
        if (steps++ == 0) {
            firstFrame = nowUs;
        }
        canvas->fill(color);
        return true;
    }
    uint64_t sinceStart() const { return elapsedUs(); }
};

static Probe *lastMade = nullptr;
static uint8_t lastParam = 0;

static const uint16_t kRed = 1, kGreen = 2, kBlue = 3;

static Animation *makeRed(Canvas *c, const Cue &cue) {
    lastParam = cue.params[0];
    return lastMade = new Probe(c, RGB(255, 0, 0));
}
static Animation *makeGreen(Canvas *c, const Cue &) { return lastMade = new Probe(c, RGB(0, 255, 0)); }
static Animation *makeBlue(Canvas *c, const Cue &) { return lastMade = new Probe(c, RGB(0, 0, 255)); }

static Canvas *makeCanvas() {
    Canvas *canvas = new Canvas(10);
    canvas->add(new Strip(0, 10));
    canvas->setup();
    return canvas;
}

static Cue effectCue(uint16_t id, uint16_t effect, uint64_t atUs) {
    Cue c;
    c.cueId = id;
    c.kind = CueKind::Effect;
    c.effect = effect;
    c.atUs = atUs;
    return c;
}

static bool canvasIs(Canvas *canvas, RGB want) {
    RGB got = canvas->get(0, 0);
    return got.r == want.r && got.g == want.g && got.b == want.b;
}

static void setup(Conductor &conductor) {
    conductor.addEffect(kRed, makeRed);
    conductor.addEffect(kGreen, makeGreen);
    conductor.addEffect(kBlue, makeBlue);
}

static void testLocalPlaylist() {
    printf("local playlist\n");
    Canvas *canvas = makeCanvas();
    FakeSource source;
    {
        Conductor conductor(canvas, source);
        setup(conductor);
        conductor.addLocal(kRed, 1000);
        conductor.addLocal(kGreen, 2000);
        conductor.init();
        CHECK(conductor.playingLocal());
        CHECK(conductor.currentEffect() == kRed);
        CHECK(alive == 1);
        uint64_t start = fakeClock;
        int greenAt = -1, redAgainAt = -1;
        for (int i = 0; i < 200; i++) {  // 5 s at 40 fps
            conductor.step();
            if (greenAt < 0 && conductor.currentEffect() == kGreen) {
                greenAt = (int)((fakeClock - start) / 1000);
            }
            if (greenAt >= 0 && redAgainAt < 0 && conductor.currentEffect() == kRed) {
                redAgainAt = (int)((fakeClock - start) / 1000);
            }
            CHECK(alive == 1);  // never more than one effect in memory
        }
        printf("  green at %d ms, red again at %d ms\n", greenAt, redAgainAt);
        CHECK(greenAt >= 1000 && greenAt <= 1050);
        CHECK(redAgainAt >= 3000 && redAgainAt <= 3075);
        CHECK(source.reports == 200);
        CHECK(source.reportedCue == 0);
    }
    CHECK(alive == 0);  // the Conductor deletes the last one
}

static void testCueTiming() {
    printf("cues land on the first frame at or after their time\n");
    Canvas *canvas = makeCanvas();
    FakeSource source;
    source.locked = true;
    source.offsetUs = 123456789;  // show time is far from this Pico's clock
    Conductor conductor(canvas, source);
    setup(conductor);
    conductor.addLocal(kRed, 60000);
    conductor.init();
    for (int i = 0; i < 5; i++) {
        conductor.step();
    }
    CHECK(canvasIs(canvas, RGB(255, 0, 0)));

    // Due 300 ms out, not on a frame boundary (frames are every 25 ms).
    uint64_t at = source.show() + 300000 + 7;
    Cue cue = effectCue(41, kBlue, at);
    source.cues.push_back(cue);
    uint64_t firstBlueFrame = 0;
    for (int i = 0; i < 40; i++) {
        conductor.step();
        if (conductor.currentEffect() == kBlue && firstBlueFrame == 0) {
            firstBlueFrame = lastMade->firstFrame;
        } else if (firstBlueFrame == 0) {
            CHECK(conductor.getFrameTime() < at);  // not early
        }
        CHECK(alive == 1);
    }
    uint64_t want = at - at % 25000 + 25000;
    printf("  cue at %llu, first blue frame at %llu (want %llu)\n", (unsigned long long)at,
           (unsigned long long)firstBlueFrame, (unsigned long long)want);
    CHECK(firstBlueFrame == want);
    CHECK(!conductor.playingLocal());
    CHECK(conductor.currentCueId() == 41);
    CHECK(source.reportedEffect == kBlue);
    CHECK(source.reportedCue == 41);
    CHECK(canvasIs(canvas, RGB(0, 0, 255)));

    // A late cue (its time already passed) plays on the very next frame.
    source.cues.push_back(effectCue(42, kGreen, source.show() - 50000));
    conductor.step();
    CHECK(conductor.currentEffect() == kGreen);

    // Two cues arriving together play in time order, whatever order they came in.
    uint64_t now = source.show();
    source.cues.push_back(effectCue(44, kBlue, now + 200000));
    source.cues.push_back(effectCue(43, kRed, now + 100000));
    for (int i = 0; i < 6; i++) {
        conductor.step();
    }
    CHECK(conductor.currentCueId() == 43);
    for (int i = 0; i < 6; i++) {
        conductor.step();
    }
    CHECK(conductor.currentCueId() == 44);

    // Cue parameters reach the factory.
    Cue p = effectCue(45, kRed, source.show());
    p.params[0] = 77;
    source.cues.push_back(p);
    conductor.step();
    CHECK(lastParam == 77);

    // An effect this prop doesn't have is skipped and the current one keeps playing.
    source.cues.push_back(effectCue(46, 0x1234, source.show()));
    conductor.step();
    CHECK(conductor.currentEffect() == kRed);
    CHECK(conductor.currentCueId() == 45);

    // Blackout.
    Cue b;
    b.cueId = 47;
    b.kind = CueKind::Blackout;
    b.atUs = source.show();
    source.cues.push_back(b);
    conductor.step();
    CHECK(conductor.currentEffect() == (uint16_t)EffectId::None);
    CHECK(canvasIs(canvas, RGB(0, 0, 0)));
    CHECK(alive == 0);  // the blackout isn't a Probe

    // Back to the local playlist.
    Cue r;
    r.cueId = 48;
    r.kind = CueKind::ResumeLocal;
    r.atUs = source.show();
    source.cues.push_back(r);
    conductor.step();
    CHECK(conductor.playingLocal());
    CHECK(conductor.currentEffect() == kRed);
}

static void testBrightness() {
    printf("brightness ramps\n");
    Canvas *canvas = makeCanvas();
    FakeSource source;
    source.locked = true;
    Conductor conductor(canvas, source);
    setup(conductor);
    conductor.addLocal(kRed, 60000);
    conductor.init();
    canvas->getRenderer()->setBrightness(40);
    conductor.step();

    Cue c;
    c.kind = CueKind::Brightness;
    c.atUs = source.show() + 100000;
    c.transitionMs = 1000;
    c.params[0] = 240;
    source.cues.push_back(c);
    uint8_t atHalf = 0;
    for (int i = 0; i < 80; i++) {
        conductor.step();
        uint64_t into = conductor.getFrameTime() - c.atUs;
        if (conductor.getFrameTime() < c.atUs) {
            CHECK(conductor.brightness() == 40);
        } else if (into == 500000) {
            atHalf = conductor.brightness();
        }
    }
    printf("  halfway %u, end %u\n", atHalf, conductor.brightness());
    CHECK(atHalf >= 138 && atHalf <= 142);
    CHECK(conductor.brightness() == 240);
    CHECK(conductor.currentEffect() == kRed);  // a brightness cue doesn't change the effect
}

static void testClockJump() {
    printf("locking to the leader restarts the effect at show time\n");
    Canvas *canvas = makeCanvas();
    FakeSource source;
    Conductor conductor(canvas, source);
    setup(conductor);
    conductor.addLocal(kGreen, 60000);
    conductor.init();
    for (int i = 0; i < 40; i++) {
        conductor.step();
    }
    Probe *p = lastMade;
    CHECK(p->sinceStart() >= 900000);

    // Locks: show time is an hour ahead of this Pico's clock.
    source.locked = true;
    source.offsetUs = 3600000000ll;
    conductor.step();
    CHECK(lastMade == p);  // same effect, not a new one
    printf("  after lock: %llu us into the effect\n", (unsigned long long)p->sinceStart());
    CHECK(p->sinceStart() < 25000);
    CHECK(conductor.getFrameTime() % 25000 == 0);

    // A small sync correction doesn't restart it.
    for (int i = 0; i < 20; i++) {
        conductor.step();
    }
    uint64_t before = p->sinceStart();
    source.offsetUs -= 5000;
    conductor.step();
    CHECK(p->sinceStart() >= before);

    // A leader reboot (show time starting over) does.
    for (int i = 0; i < 20; i++) {
        conductor.step();
    }
    source.offsetUs = -(int64_t)fakeClock + 2000000;
    conductor.step();
    CHECK(p->sinceStart() < 25000);
    CHECK(conductor.getMissedFrames() <= 1);
}

static void testFrameStats() {
    printf("frame stats\n");
    Canvas *canvas = makeCanvas();
    FakeSource source;
    {
        Conductor conductor(canvas, source);
        setup(conductor);
        conductor.addLocal(kRed, 60000);
        conductor.init();
        for (int i = 0; i < 200; i++) {  // 5 s at 40 fps
            conductor.step();
        }
        // About once a second, the first one on the first frame.
        CHECK(source.statsReports >= 5 && source.statsReports <= 6);
        CHECK_EQ(source.lastStats.fps, 40);
        CHECK(source.lastStats.frameCount >= 160 && source.lastStats.frameCount <= 200);
        CHECK(source.lastStats.missedFrames <= 1);
        CHECK(source.lastStats.worstFrameUs >= source.lastStats.avgFrameUs ||
              source.lastStats.avgFrameUs == 0);
    }
}

int main() {
    setvbuf(stdout, nullptr, _IONBF, 0);
    testLocalPlaylist();
    testCueTiming();
    testBrightness();
    testClockJump();
    testFrameStats();
    return check::finish("test_conductor");
}
