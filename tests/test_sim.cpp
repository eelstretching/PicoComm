// Whole-protocol simulation (stage 0 of the test plan in docs/protocol-design.md): one Leader and
// several Followers, each with its own crystal (boot time and drift), talking over a simulated
// WiFi network with variable, asymmetric delays, occasional slow packets, and packet loss.
//
// It checks the things the basement tests will check for real: clocks agree to well under 1 ms,
// cues arrive exactly once and in time despite loss, late joiners catch up, and followers survive
// the leader disappearing or rebooting. The election tests at the end run every node as an Elector.

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <queue>
#include <random>
#include <vector>

#include "check.h"
#include "picocomm/elector.h"
#include "picocomm/follower.h"
#include "picocomm/leader.h"

using namespace picocomm;

namespace {

bool verbose = false;
uint32_t seedBase = 0;  // PICOCOMM_SIM_SEED, to try other random networks

void logLine(const char *line) {
    if (verbose) {
        printf("    %s\n", line);
    }
}

uint32_t ipFor(int n) { return 0x0000000a | ((uint32_t)n << 24); }  // 10.0.0.n

class Sim;

// A node on the simulated network with its own clock.
class Endpoint : public Transport {
   public:
    Endpoint(Sim &sim, int n, double driftPpm, int64_t localAtBoot);
    virtual ~Endpoint() = default;
    virtual void receive(const uint8_t *pkt, uint32_t srcIp, uint16_t srcPort, uint64_t rxUs) = 0;
    virtual void tick() = 0;

    uint64_t localAt(int64_t trueUs) const {
        return (uint64_t)(localAtBoot_ + (int64_t)((trueUs - bootTrue_) * (1.0 + driftPpm_ * 1e-6)));
    }
    void reboot(int64_t trueNow) {
        bootTrue_ = trueNow;
        localAtBoot_ = 0;
    }

    uint64_t nowUs() override;
    void send(uint32_t ipv4, uint16_t port, const uint8_t *packet) override;
    void broadcast(uint16_t port, const uint8_t *packet) override;

    uint32_t ip;
    uint16_t port = kPort;
    bool up = true;

   protected:
    Sim &sim_;
    double driftPpm_;
    int64_t bootTrue_;
    int64_t localAtBoot_;
};

class Sim {
   public:
    explicit Sim(uint32_t seed) : rng(seed + seedBase) {}

    int64_t now = 0;
    std::mt19937 rng;
    double loss = 0.0;           // unicast packet loss, each direction
    double broadcastLoss = 0.1;  // broadcast has no link-layer retries
    std::vector<Endpoint *> nodes;

    int64_t unicastDelay() {
        std::exponential_distribution<double> jitter(1.0 / 1200.0);
        std::uniform_real_distribution<double> u(0, 1);
        double d = 800 + jitter(rng);
        if (u(rng) < 0.05) {
            d += u(rng) * 30000;  // contention, link-layer retries
        }
        return (int64_t)d;
    }

    void deliver(Endpoint *from, uint32_t toIp, uint16_t toPort, const uint8_t *packet,
                 bool isBroadcast) {
        std::uniform_real_distribution<double> u(0, 1);
        for (Endpoint *to : nodes) {
            if (to == from || (!isBroadcast && to->ip != toIp) || to->port != toPort) {
                continue;
            }
            if (u(rng) < (isBroadcast ? broadcastLoss : loss)) {
                continue;
            }
            int64_t at = now + unicastDelay() + (isBroadcast ? 5000 : 0);
            std::vector<uint8_t> copy(packet, packet + kPacketSize);
            uint32_t srcIp = from->ip;
            uint16_t srcPort = from->port;
            schedule(at, [to, copy, srcIp, srcPort, at]() {
                if (to->up) {
                    to->receive(copy.data(), srcIp, srcPort, to->localAt(at));
                }
            });
        }
    }

    void schedule(int64_t at, std::function<void()> fn) { queue_.push({at, order_++, std::move(fn)}); }

    // Runs for durationUs, ticking every node once per simulated millisecond.
    void run(int64_t durationUs, const std::function<void()> &everyMs = {}) {
        int64_t end = now + durationUs;
        while (now < end) {
            int64_t tickAt = now + 1000;
            while (!queue_.empty() && queue_.top().at <= tickAt) {
                Event e = queue_.top();
                queue_.pop();
                now = e.at;
                e.fn();
            }
            now = tickAt;
            for (Endpoint *n : nodes) {
                if (n->up) {
                    n->tick();
                }
            }
            if (everyMs) {
                everyMs();
            }
        }
    }

   private:
    struct Event {
        int64_t at;
        uint64_t order;
        std::function<void()> fn;
        bool operator<(const Event &o) const { return at != o.at ? at > o.at : order > o.order; }
    };
    std::priority_queue<Event> queue_;
    uint64_t order_ = 0;
};

Endpoint::Endpoint(Sim &sim, int n, double driftPpm, int64_t localAtBoot)
    : ip(ipFor(n)), sim_(sim), driftPpm_(driftPpm), bootTrue_(sim.now), localAtBoot_(localAtBoot) {
    sim.nodes.push_back(this);
}

uint64_t Endpoint::nowUs() { return localAt(sim_.now); }
void Endpoint::send(uint32_t ipv4, uint16_t p, const uint8_t *packet) {
    sim_.deliver(this, ipv4, p, packet, false);
}
void Endpoint::broadcast(uint16_t p, const uint8_t *packet) { sim_.deliver(this, 0, p, packet, true); }

struct Delivered {
    Cue cue;
    int64_t trueUs;
};

class SimLeader : public Endpoint, public LeaderApp {
   public:
    SimLeader(Sim &sim, int n, double driftPpm, int64_t localAtBoot, uint32_t generation)
        : Endpoint(sim, n, driftPpm, localAtBoot) {
        start(generation);
    }
    void start(uint32_t generation) {
        LeaderConfig cfg;
        cfg.generation = generation;
        cfg.propType = PropType::MegaTree;
        cfg.log = logLine;
        leader = std::make_unique<Leader>(cfg, *this, *this);
        leader->setOwnAddress(ip);
    }
    void rebootAs(uint32_t generation) {
        reboot(sim_.now);
        start(generation);
    }
    void receive(const uint8_t *pkt, uint32_t srcIp, uint16_t srcPort, uint64_t rxUs) override {
        leader->onPacket(pkt, kPacketSize, srcIp, srcPort, rxUs);
    }
    void tick() override { leader->poll(); }
    void deliverCue(const Cue &c) override { delivered.push_back({c, sim_.now}); }

    std::unique_ptr<Leader> leader;
    std::vector<Delivered> delivered;
};

class SimFollower : public Endpoint, public FollowerApp {
   public:
    SimFollower(Sim &sim, int n, double driftPpm, int64_t localAtBoot, PropType type, uint8_t index)
        : Endpoint(sim, n, driftPpm, localAtBoot) {
        FollowerConfig cfg;
        for (int i = 0; i < 8; i++) {
            cfg.boardId[i] = (uint8_t)(n * 16 + i);
        }
        cfg.propType = type;
        cfg.propIndex = index;
        cfg.fwVersion = 1;
        cfg.log = logLine;
        follower = std::make_unique<Follower>(cfg, *this, *this);
    }
    void receive(const uint8_t *pkt, uint32_t srcIp, uint16_t srcPort, uint64_t rxUs) override {
        follower->onPacket(pkt, kPacketSize, srcIp, srcPort, rxUs);
    }
    void tick() override {
        follower->poll();
        // Show time must never run backwards between frames (except across a leader reboot).
        if (follower->locked()) {
            uint64_t show = follower->showTime(nowUs());
            if (haveLastShow && show < lastShow && !clockWasLost) {
                backwards++;
            }
            lastShow = show;
            haveLastShow = true;
            clockWasLost = false;
        }
    }
    bool knowsEffect(uint16_t effect) override { return effect != unknownEffect; }
    void deliverCue(const Cue &c) override { delivered.push_back({c, sim_.now}); }
    void clockChanged(const ClockModel &) override { clockChanges++; }
    void clockLost() override { clockWasLost = true; }
    void fillStatus(Status &s) override {
        s.fps = 40;
        s.rssi = -60;
    }
    // True-time error of this follower's show clock against the leader's, in microseconds.
    int64_t errorUs(SimLeader &l) const {
        return (int64_t)(follower->showTime(localAt(sim_.now)) - l.localAt(sim_.now));
    }

    std::unique_ptr<Follower> follower;
    std::vector<Delivered> delivered;
    uint16_t unknownEffect = 0xffff;
    int clockChanges = 0;
    int backwards = 0;
    uint64_t lastShow = 0;
    bool haveLastShow = false;
    bool clockWasLost = false;
};

// Plays the laptop: sends QUERY and OVERRIDE, collects what comes back.
class SimObserver : public Endpoint {
   public:
    SimObserver(Sim &sim, int n) : Endpoint(sim, n, 0, 0) { port = 50000; }
    void receive(const uint8_t *pkt, uint32_t, uint16_t, uint64_t) override {
        Header h;
        if (!decodeHeader(pkt, kPacketSize, h)) {
            return;
        }
        if (h.type == PacketType::RosterEntry) {
            RosterEntry e;
            decode(pkt, e);
            roster.push_back(e);
        } else if (h.type == PacketType::Ack) {
            Ack a;
            decode(pkt, a);
            acks.push_back(a);
        }
    }
    void tick() override {}
    void query(uint32_t leaderIp) {
        uint8_t pkt[kPacketSize];
        Header h;
        h.src = kObserverId;
        encode(h, Query(), pkt);
        send(leaderIp, kPort, pkt);
    }
    void override(uint32_t leaderIp, const Override &o) {
        uint8_t pkt[kPacketSize];
        Header h;
        h.src = kObserverId;
        h.seq = 77;
        encode(h, o, pkt);
        send(leaderIp, kPort, pkt);
    }
    std::vector<RosterEntry> roster;
    std::vector<Ack> acks;
};

Cue effectCue(EffectId e) {
    Cue c;
    c.kind = CueKind::Effect;
    c.effect = (uint16_t)e;
    return c;
}

// Max |error| over all locked followers, sampled every ms from now for durationUs.
int64_t worstError(Sim &sim, SimLeader &leader, std::vector<SimFollower *> fs, int64_t durationUs,
                   int *unlockedSamples = nullptr) {
    int64_t worst = 0;
    int64_t secWorst = 0, secTarget = 0;
    int ms = 0;
    sim.run(durationUs, [&]() {
        if (verbose && ++ms % 1000 == 0) {
            printf("      t=%lld s: worst %lld us, target %lld us, drift %d ppb\n",
                   (long long)(sim.now / 1000000), (long long)secWorst, (long long)secTarget,
                   fs[0]->follower->clock().target().driftPpb);
            secWorst = secTarget = 0;
        }
        for (SimFollower *f : fs) {
            if (!f->follower->locked()) continue;
            int64_t e = f->errorUs(leader);
            if (e < 0) e = -e;
            if (e > secWorst) secWorst = e;
            int64_t te = (int64_t)(f->follower->clock().target().toShow(f->localAt(sim.now)) - leader.localAt(sim.now));
            if (te < 0) te = -te;
            if (te > secTarget) secTarget = te;
        }
        for (SimFollower *f : fs) {
            if (!f->follower->locked()) {
                if (unlockedSamples) {
                    (*unlockedSamples)++;
                }
                continue;
            }
            int64_t e = f->errorUs(leader);
            if (e < 0) {
                e = -e;
            }
            if (e > worst) {
                worst = e;
            }
        }
    });
    return worst;
}

const int64_t kSec = 1000000;

void testSyncAndCues() {
    printf("  sync and cues, 4 followers, no loss\n");
    Sim sim(1);
    SimLeader leader(sim, 1, 0, 50 * kSec, 0x1111);
    SimFollower a(sim, 2, +28, 3 * kSec, PropType::Arch, 1);
    SimFollower b(sim, 3, -31, 900 * kSec, PropType::Arch, 2);
    SimFollower c(sim, 4, +12, 7 * kSec, PropType::Topper, 1);
    SimFollower d(sim, 5, -5, 12345678, PropType::MiniTree, 1);
    std::vector<SimFollower *> all{&a, &b, &c, &d};

    sim.run(5 * kSec);
    for (SimFollower *f : all) {
        CHECK(f->follower->state() == Follower::State::Joined);
        CHECK(f->follower->locked());
    }
    CHECK_EQ(leader.leader->activeFollowers(), 4);

    // Let the drift fit fill up, then measure for a minute.
    sim.run(30 * kSec);
    int64_t worst = worstError(sim, leader, all, 60 * kSec);
    printf("    worst clock error over 60 s: %lld us\n", (long long)worst);
    CHECK(worst < 500);

    // Scene change: arches get Leap, the topper Starlight, same instant.
    uint16_t id1 = leader.leader->cue(groupBit(PropType::Arch), effectCue(EffectId::Leap));
    uint16_t id2 = leader.leader->cue(groupBit(PropType::Topper) | groupBit(PropType::MegaTree),
                                      effectCue(EffectId::Starlight));
    sim.run(1 * kSec);
    CHECK_EQ(a.delivered.size(), 1u);
    CHECK_EQ(b.delivered.size(), 1u);
    CHECK_EQ(c.delivered.size(), 1u);
    CHECK_EQ(d.delivered.size(), 0u);
    CHECK_EQ(leader.delivered.size(), 1u);  // the leader is the MegaTree
    if (a.delivered.size() == 1 && b.delivered.size() == 1 && c.delivered.size() == 1) {
        CHECK_EQ(a.delivered[0].cue.cueId, id1);
        CHECK_EQ(a.delivered[0].cue.effect, (uint16_t)EffectId::Leap);
        CHECK_EQ(c.delivered[0].cue.cueId, id2);
        CHECK(a.delivered[0].cue.atUs == b.delivered[0].cue.atUs);
        // Delivered ahead of time.
        CHECK(a.delivered[0].trueUs < sim.now);
    }
    for (SimFollower *f : all) {
        CHECK_EQ(f->follower->stats().cuesLate, 0u);
        CHECK_EQ(f->backwards, 0);
    }
    for (int i = 0; i < leader.leader->memberSlots(); i++) {
        const Leader::Member &m = leader.leader->member(i);
        if (m.used) {
            CHECK_EQ(m.cuesMissed, 0u);
            CHECK_EQ(m.cuesLate, 0u);
            for (const Leader::PendingCue &p : m.pending) {
                CHECK(!p.used);
            }
        }
    }

    // Observer: roster and an override.
    SimObserver obs(sim, 9);
    obs.query(leader.ip);
    sim.run(200000);
    CHECK_EQ(obs.roster.size(), 5u);
    // Rows can arrive out of order (each takes its own path through the air); entryIndex says
    // where each belongs.
    std::sort(obs.roster.begin(), obs.roster.end(),
              [](const RosterEntry &x, const RosterEntry &y) { return x.entryIndex < y.entryIndex; });
    if (!obs.roster.empty()) {
        CHECK_EQ(obs.roster[0].nodeId, kLeaderId);
        CHECK_EQ(obs.roster[0].entryCount, 5);
        CHECK(obs.roster[1].state == NodeState::Synced);
        CHECK_EQ(obs.roster[1].fps, 40);
        CHECK(obs.roster[1].syncDelayUs > 0);
    }
    Override o;
    o.targetGroupMask = groupBit(PropType::MiniTree);
    o.kind = CueKind::Effect;
    o.effect = (uint16_t)EffectId::TwinkleFox;
    o.delayMs = 500;
    obs.override(leader.ip, o);
    sim.run(1 * kSec);
    CHECK_EQ(obs.acks.size(), 1u);
    CHECK_EQ(d.delivered.size(), 1u);
    if (obs.acks.size() == 1 && d.delivered.size() == 1) {
        CHECK(obs.acks[0].ackedType == PacketType::Override);
        CHECK_EQ(obs.acks[0].ackedSeq, 77);
        CHECK_EQ(obs.acks[0].cueId, d.delivered[0].cue.cueId);
        CHECK_EQ(d.delivered[0].cue.effect, (uint16_t)EffectId::TwinkleFox);
    }
    // The same OVERRIDE again (same seq: the observer missed the ACK) is ACKed, not re-cued.
    obs.override(leader.ip, o);
    sim.run(1 * kSec);
    CHECK_EQ(obs.acks.size(), 2u);
    CHECK_EQ(d.delivered.size(), 1u);
    if (obs.acks.size() == 2) {
        CHECK_EQ(obs.acks[1].cueId, obs.acks[0].cueId);
    }
}

void testLossyNetwork() {
    printf("  30%% packet loss each way\n");
    Sim sim(2);
    sim.loss = 0.30;
    sim.broadcastLoss = 0.4;
    SimLeader leader(sim, 1, 0, 1 * kSec, 0x2222);
    std::vector<SimFollower *> all;
    std::vector<std::unique_ptr<SimFollower>> owned;
    for (int i = 0; i < 6; i++) {
        owned.push_back(std::make_unique<SimFollower>(sim, 2 + i, (i % 2 ? 1 : -1) * (10.0 + 4 * i),
                                                      (i + 1) * 7 * kSec, PropType::Arch, i + 1));
        all.push_back(owned.back().get());
    }
    sim.run(40 * kSec);
    int64_t worst = worstError(sim, leader, all, 30 * kSec);
    printf("    worst clock error over 30 s: %lld us\n", (long long)worst);
    CHECK(worst < 1000);

    // Twenty scene changes, 3 s apart.
    for (int i = 0; i < 20; i++) {
        leader.leader->cue(groupBit(PropType::Arch), effectCue(i % 2 ? EffectId::Leap : EffectId::Cylon));
        sim.run(3 * kSec);
    }
    sim.run(3 * kSec);
    uint32_t late = 0, missed = 0;
    for (SimFollower *f : all) {
        CHECK_EQ(f->delivered.size(), 20u);  // each exactly once, despite resends
        late += f->follower->stats().cuesLate;
        CHECK_EQ(f->backwards, 0);
    }
    for (int i = 0; i < leader.leader->memberSlots(); i++) {
        if (leader.leader->member(i).used) {
            missed += leader.leader->member(i).cuesMissed;
        }
    }
    printf("    120 cues: %u late, %u missed\n", late, missed);
    CHECK_EQ(missed, 0u);
    CHECK(late <= 3);
}

void testLateJoinerAndUnknownEffect() {
    printf("  late joiner, unknown effect\n");
    Sim sim(3);
    SimLeader leader(sim, 1, 0, 1 * kSec, 0x3333);
    SimFollower a(sim, 2, 20, 2 * kSec, PropType::Arch, 1);
    sim.run(5 * kSec);
    uint16_t id = leader.leader->cue(groupBit(PropType::Arch), effectCue(EffectId::Leap));
    sim.run(5 * kSec);

    SimFollower late(sim, 3, -20, 0, PropType::Arch, 2);
    sim.run(5 * kSec);
    CHECK_EQ(late.delivered.size(), 1u);
    if (!late.delivered.empty()) {
        CHECK_EQ(late.delivered[0].cue.cueId, id);
    }
    // Lateness of a catch-up cue is expected and not counted against the follower.
    for (int i = 0; i < leader.leader->memberSlots(); i++) {
        if (leader.leader->member(i).used) {
            CHECK_EQ(leader.leader->member(i).cuesLate, 0u);
        }
    }

    a.unknownEffect = (uint16_t)EffectId::Cylon;
    leader.leader->cue(groupBit(PropType::Arch), effectCue(EffectId::Cylon));
    sim.run(1 * kSec);
    CHECK_EQ(a.delivered.size(), 1u);     // refused, still playing Leap
    CHECK_EQ(late.delivered.size(), 2u);  // this one knows Cylon
    CHECK_EQ(leader.leader->member(0).cuesUnknown, 1u);
}

void testLeaderOutageAndReboot() {
    printf("  leader outage, then leader reboot\n");
    Sim sim(4);
    SimLeader leader(sim, 1, 0, 100 * kSec, 0x4444);
    SimFollower a(sim, 2, 25, 3 * kSec, PropType::Arch, 1);
    SimFollower b(sim, 3, -25, 4 * kSec, PropType::Arch, 2);
    std::vector<SimFollower *> all{&a, &b};
    sim.run(30 * kSec);
    leader.leader->cue(groupBit(PropType::Arch), effectCue(EffectId::Leap));
    sim.run(2 * kSec);

    // The router goes away for 15 s: followers orphan and fall back to their local playlist.
    leader.up = false;
    sim.run(15 * kSec);
    for (SimFollower *f : all) {
        CHECK(f->follower->orphaned());
        CHECK(!f->delivered.empty() && f->delivered.back().cue.kind == CueKind::ResumeLocal);
        CHECK(f->follower->locked());  // still free-running on the last fit
    }
    // It comes back (same generation): they rejoin and get the current scene again.
    leader.up = true;
    sim.run(5 * kSec);
    for (SimFollower *f : all) {
        CHECK(!f->follower->orphaned());
        CHECK(f->follower->state() == Follower::State::Joined);
        CHECK(!f->delivered.empty() && f->delivered.back().cue.effect == (uint16_t)EffectId::Leap);
    }
    int64_t worst = worstError(sim, leader, all, 20 * kSec);
    printf("    worst clock error after the outage: %lld us\n", (long long)worst);
    CHECK(worst < 500);

    // The leader reboots: new generation, show time starts over from zero.
    leader.rebootAs(0x5555);
    sim.run(5 * kSec);
    for (SimFollower *f : all) {
        CHECK_EQ(f->follower->stats().leaderReboots, 1u);
        CHECK(f->follower->leaderGeneration() == 0x5555);
        CHECK(f->follower->locked());
    }
    sim.run(30 * kSec);
    worst = worstError(sim, leader, all, 20 * kSec);
    printf("    worst clock error after the reboot: %lld us\n", (long long)worst);
    CHECK(worst < 500);
    // Cue IDs restart after a reboot; the followers must not treat them as duplicates.
    size_t before = a.delivered.size();
    leader.leader->cue(groupBit(PropType::Arch), effectCue(EffectId::Cylon));
    sim.run(1 * kSec);
    CHECK_EQ(a.delivered.size(), before + 1);
    CHECK_EQ(a.backwards, 0);
}

void testFollowerReboot() {
    printf("  follower reboot, stale and lost\n");
    Sim sim(5);
    SimLeader leader(sim, 1, 0, 1 * kSec, 0x6666);
    auto a = std::make_unique<SimFollower>(sim, 2, 15, 2 * kSec, PropType::Arch, 1);
    sim.run(10 * kSec);
    uint8_t node = a->follower->nodeId();
    CHECK(node != kUnassignedId);

    a->up = false;
    sim.run(8 * kSec);
    CHECK(leader.leader->member(node - 1).state == NodeState::Stale);
    sim.run(25 * kSec);
    CHECK(leader.leader->member(node - 1).state == NodeState::Lost);
    CHECK_EQ(leader.leader->activeFollowers(), 0);

    // Same board comes back as a fresh process: it gets its old node ID.
    sim.nodes.erase(std::find(sim.nodes.begin(), sim.nodes.end(), a.get()));
    a = std::make_unique<SimFollower>(sim, 2, 15, 0, PropType::Arch, 1);
    sim.run(5 * kSec);
    CHECK_EQ(a->follower->nodeId(), node);
    CHECK(leader.leader->member(node - 1).state == NodeState::Synced);
    CHECK_EQ(leader.leader->activeFollowers(), 1);
}

// ---- Leader election ----

std::mt19937 electionRng(99);
uint32_t electionRandom() { return (uint32_t)electionRng(); }

// A prop running an Elector: leads or follows as the election goes.
class SimNode : public Endpoint, public ElectorApp {
   public:
    SimNode(Sim &sim, int n, double driftPpm, int64_t localAtBoot, uint32_t priority,
            PropType type = PropType::Arch, uint8_t index = 1)
        : Endpoint(sim, n, driftPpm, localAtBoot), priority_(priority), type_(type), index_(index) {
        start();
    }
    void start() {
        ElectorConfig cfg;
        cfg.priority = priority_;
        for (int i = 0; i < 8; i++) {
            cfg.boardId[i] = (uint8_t)((ip >> 24) * 16 + i);
        }
        cfg.propType = type_;
        cfg.propIndex = index_;
        cfg.ownIp = ip;
        cfg.random32 = electionRandom;
        cfg.log = logLine;
        published = false;
        elector.reset();
        elector = std::make_unique<Elector>(cfg, *this, *this);
    }
    // Power cycle: the clock starts over at zero and so does the Elector.
    void powerCycle() {
        reboot(sim_.now);
        start();
    }
    void receive(const uint8_t *pkt, uint32_t srcIp, uint16_t srcPort, uint64_t rxUs) override {
        elector->onPacket(pkt, kPacketSize, srcIp, srcPort, rxUs);
    }
    void tick() override {
        elector->poll();
        uint64_t show;
        if (showTime(show)) {
            if (haveLast && show < lastShow) {
                backwards++;
            }
            lastShow = show;
            haveLast = true;
        }
    }
    // What core 0 would read: the published clock, whichever role this node has.
    bool showTime(uint64_t &show) {
        if (!published) {
            return false;
        }
        show = model.toShow(nowUs());
        return true;
    }
    bool leading() const { return elector->leading(); }

    bool knowsEffect(uint16_t) override { return true; }
    void deliverCue(const Cue &c) override { delivered.push_back({c, sim_.now}); }
    void clockChanged(const ClockModel &m) override {
        model = m;
        published = true;
    }
    void clockLost() override {
        published = false;
        clockLosses++;
        heard = {};
    }
    void leadingChanged(bool leading) override {
        roleChanges++;
        uint64_t show;
        if (leading && hasRtc && showTime(show)) {
            // The DS3231 (whose seconds tick can be caught exactly): the true time now.
            own = {trueUnixUs(sim_), show};
        }
    }
    void wallClockHeard(uint64_t unixUs, uint64_t show) override { heard = {unixUs, show}; }
    // Like Node::unixTimeUs: following, the leader's wall clock wins; leading, our own RTC, or
    // the one heard from the leader before us.
    uint64_t unixTimeUs(uint64_t show) override {
        const WallClock *w = leading() ? (own.us ? &own : (heard.us ? &heard : nullptr))
                                       : (heard.us ? &heard : (own.us ? &own : nullptr));
        return w == nullptr ? 0 : w->us + (show - w->show);
    }
    // Unix time now by this node's own reckoning, in microseconds, or 0.
    uint64_t unixNowUs() {
        uint64_t show;
        return showTime(show) ? unixTimeUs(show) : 0;
    }
    static uint64_t trueUnixUs(const Sim &sim) { return 1797000000ull * 1000000 + (uint64_t)sim.now; }

    struct WallClock {
        uint64_t us = 0;
        uint64_t show = 0;
    };
    bool hasRtc = false;
    WallClock own, heard;

    std::unique_ptr<Elector> elector;
    std::vector<Delivered> delivered;
    ClockModel model;
    bool published = false;
    int backwards = 0;
    int clockLosses = 0;
    int roleChanges = 0;
    uint64_t lastShow = 0;
    bool haveLast = false;

   private:
    uint32_t priority_;
    PropType type_;
    uint8_t index_;
};

int leaderCount(const std::vector<SimNode *> &nodes) {
    int n = 0;
    for (SimNode *s : nodes) {
        if (s->up && s->leading()) {
            n++;
        }
    }
    return n;
}

SimNode *theLeader(const std::vector<SimNode *> &nodes) {
    for (SimNode *s : nodes) {
        if (s->up && s->leading()) {
            return s;
        }
    }
    return nullptr;
}

// Worst show-time disagreement between the leader and every locked node, sampled every ms.
int64_t worstSpread(Sim &sim, const std::vector<SimNode *> &nodes, int64_t durationUs) {
    int64_t worst = 0;
    sim.run(durationUs, [&]() {
        SimNode *l = theLeader(nodes);
        uint64_t ref;
        if (l == nullptr || !l->showTime(ref)) {
            return;
        }
        for (SimNode *s : nodes) {
            uint64_t t;
            if (s->up && s != l && s->showTime(t)) {
                int64_t e = (int64_t)(t - ref);
                if (e < 0) e = -e;
                if (e > worst) worst = e;
            }
        }
    });
    return worst;
}

void testElection() {
    printf("  election: highest priority leads, takeover carries on show time\n");
    Sim sim(6);
    // The MegaTree (6400 px) boots last; two arches (200 px each) and a mini tree that never leads.
    SimNode arch1(sim, 2, 20, 5 * kSec, 200, PropType::Arch, 1);
    SimNode arch2(sim, 3, -15, 9 * kSec, 200, PropType::Arch, 2);
    SimNode mini(sim, 4, 8, 2 * kSec, kNeverLead, PropType::MiniTree, 1);
    std::vector<SimNode *> all{&arch1, &arch2, &mini};
    sim.run(8 * kSec);
    // Equal priorities: the higher address (arch2) wins.
    CHECK_EQ(leaderCount(all), 1);
    CHECK(arch2.leading());
    CHECK(!mini.leading());

    SimNode tree(sim, 1, 0, 0, 6400, PropType::MegaTree, 1);
    all.push_back(&tree);
    sim.run(10 * kSec);
    CHECK_EQ(leaderCount(all), 1);
    CHECK(tree.leading());
    CHECK_EQ(tree.elector->takeovers(), 1u);
    CHECK_EQ(arch2.elector->stepDowns(), 1u);
    for (SimNode *s : all) {
        CHECK_EQ(s->elector->leaderIp(), s == &tree ? 0u : tree.ip);
        if (!s->leading()) {
            CHECK(s->elector->follower()->locked());
        }
    }
    sim.run(30 * kSec);
    int64_t worst = worstSpread(sim, all, 30 * kSec);
    printf("    worst spread under the MegaTree: %lld us\n", (long long)worst);
    CHECK(worst < 500);

    // The MegaTree's controller dies mid-show. The arch with the higher address takes over and
    // carries on the show time, so nothing jumps and nobody loses the clock.

    uint16_t id = tree.elector->cue(groupBit(PropType::Arch), effectCue(EffectId::Leap));
    CHECK(id != 0);
    sim.run(2 * kSec);
    tree.up = false;
    sim.run(12 * kSec);
    std::vector<SimNode *> rest{&arch1, &arch2, &mini};
    CHECK_EQ(leaderCount(rest), 1);
    CHECK(arch2.leading());
    for (SimNode *s : rest) {
        CHECK_EQ(s->clockLosses, 0);
        CHECK_EQ(s->backwards, 0);
        if (!s->leading()) {
            // A second candidate may have claimed too (its jitter ran out before the first
            // claim's beacon reached it) and stepped down; either way no one saw a reboot.
            CHECK_EQ(s->elector->follower()->stats().leaderReboots, 0u);
        }
    }
    sim.run(30 * kSec);
    worst = worstSpread(sim, rest, 20 * kSec);
    printf("    worst spread after the takeover: %lld us\n", (long long)worst);
    CHECK(worst < 500);
    // The new leader can cue the show.
    size_t before = arch1.delivered.size();
    CHECK(arch2.elector->cue(groupBit(PropType::Arch), effectCue(EffectId::Cylon)) != 0);
    sim.run(1 * kSec);
    CHECK_EQ(arch1.delivered.size(), before + 1);

    // The MegaTree reboots: it joins, locks, and takes the show back without a jump.
    tree.up = true;
    tree.powerCycle();
    sim.run(10 * kSec);
    CHECK_EQ(leaderCount(all), 1);
    CHECK(tree.leading());
    for (SimNode *s : rest) {
        CHECK_EQ(s->clockLosses, 0);
        CHECK_EQ(s->backwards, 0);
    }
    sim.run(30 * kSec);
    worst = worstSpread(sim, all, 20 * kSec);
    printf("    worst spread after the MegaTree took the show back: %lld us\n", (long long)worst);
    CHECK(worst < 500);
}

void testElectionSplitAndFixedLeader() {
    printf("  election: two leaders after a network split; fixed leader never deposed\n");
    Sim sim(7);
    SimNode a(sim, 2, 10, 1 * kSec, 300);
    SimNode b(sim, 3, -10, 2 * kSec, 500);
    SimNode f(sim, 4, 5, 3 * kSec, kNeverLead, PropType::MiniTree, 1);
    std::vector<SimNode *> all{&a, &b, &f};
    sim.run(10 * kSec);
    CHECK(b.leading());
    // b drops off the network long enough for a to take over, then comes back: two leaders until
    // a hears b's beacon and steps down.
    b.up = false;
    sim.run(12 * kSec);
    CHECK(a.leading());
    b.up = true;
    sim.run(5 * kSec);
    CHECK_EQ(leaderCount(all), 1);
    CHECK(b.leading());
    CHECK_EQ(f.elector->leaderIp(), b.ip);

    // A fixed leader with the lowest address still outranks every candidate.
    Sim sim2(8);
    SimNode fixed(sim2, 1, 0, 0, kAlwaysLead, PropType::MegaTree, 1);
    SimNode c(sim2, 9, 20, 4 * kSec, 6400);
    std::vector<SimNode *> both{&fixed, &c};
    sim2.run(60 * kSec);
    CHECK_EQ(leaderCount(both), 1);
    CHECK(fixed.leading());
    CHECK_EQ(c.elector->takeovers(), 0u);
}

void testElectionLossy() {
    printf("  election on a lossy network: no flapping\n");
    Sim sim(9);
    sim.loss = 0.30;
    sim.broadcastLoss = 0.4;
    std::vector<std::unique_ptr<SimNode>> owned;
    std::vector<SimNode *> all;
    for (int i = 0; i < 6; i++) {
        owned.push_back(std::make_unique<SimNode>(sim, 2 + i, (i % 2 ? 1 : -1) * (10.0 + 4 * i),
                                                  (i + 1) * 3 * kSec, i < 3 ? 200 : kNeverLead,
                                                  PropType::Arch, i + 1));
        all.push_back(owned.back().get());
    }
    // All boot together, so with 40% of beacons lost a few may claim before settling.
    auto takeovers = [&]() {
        uint32_t n = 0;
        for (SimNode *s : all) {
            n += s->elector->takeovers();
        }
        return n;
    };
    sim.run(20 * kSec);
    uint32_t settled = takeovers();
    int maxLeaders = 0, leaderless = 0;
    sim.run(120 * kSec, [&]() {
        int n = leaderCount(all);
        maxLeaders = std::max(maxLeaders, n);
        leaderless += n == 0;
    });
    printf("    %u claim(s) while settling, %u in the next 2 minutes\n", settled,
           takeovers() - settled);
    for (SimNode *s : all) {
        CHECK_EQ(s->backwards, 0);
    }
    CHECK(owned[2]->leading());  // the highest-addressed candidate
    CHECK_EQ(takeovers(), settled);
    CHECK_EQ(maxLeaders, 1);
    CHECK_EQ(leaderless, 0);
}

void testMixedRtc() {
    printf("  mixed RTC and no-RTC boards: an RTC board leads, the wall clock carries on\n");
    Sim sim(10);
    // The MegaTree's board has no RTC yet; one arch does. The arch leads despite fewer pixels.
    SimNode tree(sim, 1, 0, 0, candidatePriority(6400, false), PropType::MegaTree, 1);
    SimNode arch(sim, 2, 15, 4 * kSec, candidatePriority(200, true), PropType::Arch, 1);
    SimNode mini(sim, 3, -12, 7 * kSec, kNeverLead, PropType::MiniTree, 1);
    arch.hasRtc = true;
    arch.powerCycle();  // so it starts with the RTC fitted
    std::vector<SimNode *> all{&tree, &arch, &mini};
    int64_t worstWallUs = 0;
    auto wallClocksAgree = [&](const std::vector<SimNode *> &nodes) {
        // Good to 1/256 s, plus how far show time is off, plus the beacon's trip.
        bool ok = true;
        for (SimNode *s : nodes) {
            int64_t err = (int64_t)(s->unixNowUs() - SimNode::trueUnixUs(sim));
            worstWallUs = std::max(worstWallUs, err < 0 ? -err : err);
            if (err < -6000 || err > 6000) {
                printf("    node %u: wall clock off by %lld us\n", s->ip >> 24, (long long)err);
                ok = false;
            }
        }
        return ok;
    };
    CHECK(candidatePriority(200, true) > candidatePriority(6400, false));
    CHECK(candidatePriority(0, false) == 1);
    CHECK(candidatePriority(0xffffffff, true) == kAlwaysLead - 1);

    sim.run(20 * kSec);
    CHECK_EQ(leaderCount(all), 1);
    CHECK(arch.leading());
    CHECK(wallClocksAgree(all));  // the boards without an RTC learned it from the beacons

    // The arch dies. The tree takes over with no RTC of its own, and carries on both clocks.
    arch.up = false;
    sim.run(15 * kSec);
    std::vector<SimNode *> rest{&tree, &mini};
    CHECK(tree.leading());
    CHECK_EQ(tree.own.us, 0u);
    CHECK(wallClocksAgree(rest));
    CHECK_EQ(mini.clockLosses, 0);
    sim.run(60 * kSec);
    CHECK(wallClocksAgree(rest));

    // The arch comes back and takes the show back, since it has the RTC.
    arch.up = true;
    arch.powerCycle();
    sim.run(15 * kSec);
    CHECK_EQ(leaderCount(all), 1);
    CHECK(arch.leading());
    CHECK(wallClocksAgree(all));
    for (SimNode *s : all) {
        CHECK_EQ(s->backwards, 0);
    }

    printf("    worst wall-clock error: %lld us\n", (long long)worstWallUs);

    // With no RTC anywhere, show time works as before and the wall clock is simply unknown.
    Sim sim2(11);
    SimNode a(sim2, 2, 10, 1 * kSec, candidatePriority(300, false));
    SimNode f(sim2, 3, -10, 2 * kSec, kNeverLead);
    sim2.run(20 * kSec);
    CHECK(a.leading());
    CHECK_EQ(f.unixNowUs(), 0u);
    CHECK(f.elector->follower()->locked());
}

}  // namespace

int main(int argc, char **argv) {
    verbose = argc > 1 && strcmp(argv[1], "-v") == 0;
    if (const char *seed = getenv("PICOCOMM_SIM_SEED")) {
        seedBase = (uint32_t)atoi(seed);
    }
    testSyncAndCues();
    testLossyNetwork();
    testLateJoinerAndUnknownEffect();
    testLeaderOutageAndReboot();
    testFollowerReboot();
    testElection();
    testElectionSplitAndFixedLeader();
    testElectionLossy();
    testMixedRtc();
    return check::finish("test_sim");
}
