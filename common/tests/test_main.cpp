// Minimal dependency-free test harness + tests for the VRStream common core.
#include <cstdio>
#include <cstdlib>
#include <random>

#include "vrstream/congestion.h"
#include "vrstream/fec.h"
#include "vrstream/packetizer.h"
#include "vrstream/pacing.h"
#include "vrstream/protocol.h"
#include "vrstream/timesync.h"

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        g_checks++;                                                       \
        if (!(cond)) {                                                    \
            g_failures++;                                                 \
            std::printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);   \
        }                                                                 \
    } while (0)

static void testFecBasics() {
    // GF sanity: a * a^-1 == 1
    for (int a = 1; a < 256; a++) {
        CHECK(vrstream::Fec::gfMul(static_cast<uint8_t>(a),
                                   vrstream::Fec::gfInv(static_cast<uint8_t>(a))) == 1);
    }
    // Distribution: gfMul is a bijection for fixed nonzero multiplier.
    for (int a = 1; a < 256; a += 7) {
        std::vector<bool> seen(256, false);
        for (int b = 0; b < 256; b++) {
            uint8_t p = vrstream::Fec::gfMul(static_cast<uint8_t>(a), static_cast<uint8_t>(b));
            CHECK(!seen[p]);
            seen[p] = true;
        }
    }
}

static void testFecRecovery() {
    std::mt19937 rng(1234);
    for (int trial = 0; trial < 25; trial++) {
        size_t k = 2 + rng() % 40;
        size_t r = 1 + rng() % 8;
        if (k + r > 255) r = 255 - k;
        size_t len = 1 + rng() % 1500;
        std::vector<std::vector<uint8_t>> data(k);
        for (auto& d : data) {
            d.resize(len);
            for (auto& b : d) b = static_cast<uint8_t>(rng());
        }
        auto repairs = vrstream::Fec::encode(data, r);

        // Erase exactly r random data packets.
        std::vector<size_t> idx(k);
        for (size_t i = 0; i < k; i++) idx[i] = i;
        std::shuffle(idx.begin(), idx.end(), rng);
        std::vector<const std::vector<uint8_t>*> dataPtrs(k, nullptr);
        for (size_t i = 0; i < k; i++)
            if (i >= r) dataPtrs[idx[i]] = &data[idx[i]];
        std::vector<const std::vector<uint8_t>*> repairPtrs;
        for (auto& p : repairs) repairPtrs.push_back(&p);

        std::vector<std::vector<uint8_t>> recovered;
        bool ok = vrstream::Fec::decode(dataPtrs, repairPtrs, len, recovered);
        CHECK(ok);
        if (ok) {
            for (size_t i = 0; i < k; i++) {
                if (!dataPtrs[i]) CHECK(recovered[i] == data[i]);
            }
        }
    }
}

static void testFecTooManyErasures() {
    std::mt19937 rng(99);
    size_t k = 10, r = 3, len = 200;
    std::vector<std::vector<uint8_t>> data(k, std::vector<uint8_t>(len));
    for (auto& d : data)
        for (auto& b : d) b = static_cast<uint8_t>(rng());
    auto repairs = vrstream::Fec::encode(data, r);

    std::vector<const std::vector<uint8_t>*> dataPtrs(k, nullptr);
    for (size_t i = 0; i < k; i++)
        if (i >= r + 1) dataPtrs[i] = &data[i];  // lose r+1
    std::vector<const std::vector<uint8_t>*> repairPtrs;
    for (auto& p : repairs) repairPtrs.push_back(&p);

    std::vector<std::vector<uint8_t>> recovered;
    CHECK(!vrstream::Fec::decode(dataPtrs, repairPtrs, len, recovered));
}

static void testPacketizerRoundTrip() {
    std::mt19937 rng(7);
    vrstream::FrameSenderConfig cfg;
    cfg.mtu = 100;
    cfg.fecPercent = 20;
    vrstream::FrameSender sender(cfg);

    std::vector<uint8_t> frame(12345);
    for (auto& b : frame) b = static_cast<uint8_t>(rng());
    auto dgrams = sender.packetize(frame.data(), frame.size(), 7, 42, true);
    CHECK(dgrams.size() > frame.size() / 100);

    vrstream::FrameReceiver receiver;
    // No loss. A late repair packet after delivery returns nullopt, so keep
    // the last non-empty result.
    std::optional<vrstream::ReceivedFrame> got;
    for (auto& d : dgrams)
        if (auto r = receiver.ingest(d.header, d.payload.data(), d.payload.size()))
            got = r;
    CHECK(got.has_value());
    if (got) {
        CHECK(got->bytes == frame);
        CHECK(got->keyframe);
        CHECK(got->frameIndex == 7);
    }
}

static void testPacketizerFecLoss() {
    std::mt19937 rng(21);
    vrstream::FrameSenderConfig cfg;
    cfg.mtu = 100;
    cfg.fecPercent = 25;
    cfg.maxGroupSize = 32;
    vrstream::FrameSender sender(cfg);

    std::vector<uint8_t> frame(9000);
    for (auto& b : frame) b = static_cast<uint8_t>(rng());
    auto dgrams = sender.packetize(frame.data(), frame.size(), 1, 10, false);

    // Count data packets, drop a few (within FEC budget of their groups).
    std::map<std::pair<uint32_t, uint16_t>, int> perGroup;
    for (auto& d : dgrams)
        if (!d.header.isFec) perGroup[{d.header.frameIndex, d.header.groupIdx}]++;

    vrstream::FrameReceiver receiver;
    std::optional<vrstream::ReceivedFrame> got;
    int dropped = 0;
    for (auto& d : dgrams) {
        bool drop = !d.header.isFec && d.header.packetInGroup == 1 &&
                    dropped < 1;  // one data packet per group max
        if (drop) {
            dropped++;
            continue;
        }
        if (auto r = receiver.ingest(d.header, d.payload.data(), d.payload.size()))
            got = r;
    }
    CHECK(got.has_value());
    if (got) {
        CHECK(got->bytes == frame);
        CHECK(receiver.fecRecoveredFrames() >= 1);
    }
}

static void testPacketizerNackAndStale() {
    vrstream::FrameSenderConfig cfg;
    cfg.mtu = 100;
    cfg.fecPercent = 0;  // no FEC: any loss must produce NACKs
    vrstream::FrameSender sender(cfg);

    std::vector<uint8_t> frame(500);
    for (size_t i = 0; i < frame.size(); i++) frame[i] = static_cast<uint8_t>(i);
    auto dgrams = sender.packetize(frame.data(), frame.size(), 1, 1, false);

    vrstream::FrameReceiver receiver;
    std::optional<vrstream::ReceivedFrame> got;
    for (auto& d : dgrams) {
        if (!d.header.isFec && d.header.packetInGroup == 2) continue;  // lose one
        if (auto r = receiver.ingest(d.header, d.payload.data(), d.payload.size()))
            got = r;
    }
    CHECK(!got.has_value());
    auto nacks = receiver.pendingNacks(vrstream::nowUs(), 100000);
    CHECK(nacks.size() == 1);
    CHECK(nacks[0].packetIdx == 2);

    // A later complete frame delivers and the stale frame is dropped.
    std::vector<uint8_t> frame2(500);
    for (size_t i = 0; i < frame2.size(); i++) frame2[i] = static_cast<uint8_t>(0xAA);
    auto dgrams2 = sender.packetize(frame2.data(), frame2.size(), 2, 2, false);
    for (auto& d : dgrams2)
        if (auto r = receiver.ingest(d.header, d.payload.data(), d.payload.size()))
            got = r;
    CHECK(got.has_value());
    if (got) CHECK(got->frameIndex == 2);
    CHECK(receiver.staleDroppedFrames() >= 1);
}

static void testPacer() {
    vrstream::Pacer p;
    p.configure(11111, 0.6);
    p.setPacketsPerFrame(100);
    uint64_t t0 = 1000000;
    CHECK(p.sendTimeUs(t0, 0) == t0);
    uint64_t last = 0;
    for (size_t i = 0; i < 100; i++) {
        uint64_t t = p.sendTimeUs(t0, i);
        CHECK(t >= t0);
        CHECK(t <= t0 + 11111 * 0.6 + 1);
        CHECK(t >= last);
        last = t;
    }
    CHECK(vrstream::Pacer::waitUs(100, 90) == 0);
    CHECK(vrstream::Pacer::waitUs(90, 100) == 10);
}

static void testTimesync() {
    // Server clock runs +5000us relative to client, symmetric 2000us one-way
    // delay, 50us server turnaround.
    vrstream::ClockSync sync;
    int64_t offset = 5000;
    uint64_t delay = 2000;
    for (int i = 0; i < 10; i++) {
        uint64_t t1 = 1'000'000 + i * 100'000;
        uint64_t t2 = t1 + delay + offset;
        uint64_t t3 = t2 + 50;
        uint64_t t4 = t1 + 2 * delay + 50;
        sync.addSample({t1, t2, t3, t4});
    }
    int64_t est = 0;
    CHECK(sync.offsetUs(est));
    CHECK(est > offset - 100 && est < offset + 100);
    CHECK(sync.bestRttUs() == 2 * delay);
}

static void testCongestion() {
    vrstream::CongestionController::Config c;
    c.minBitrateBps = 20'000'000;
    c.maxBitrateBps = 400'000'000;
    c.startBitrateBps = 100'000'000;
    vrstream::CongestionController cc(c);

    // Clean link ramps to max.
    vrstream::CongestionInputs clean;
    for (int i = 0; i < 200; i++) cc.update(clean);
    CHECK(cc.bitrate() == c.maxBitrateBps);

    // Loss spike backs off hard.
    vrstream::CongestionInputs lossy;
    lossy.lossPercent = 10.f;
    cc.update(lossy);
    for (int i = 0; i < 5; i++) cc.update(lossy);
    CHECK(cc.bitrate() < 100'000'000);

    // Decoder backlog cuts too.
    vrstream::CongestionInputs backlog;
    backlog.decodeBacklog = 3;
    cc.forceBitrate(200'000'000);
    for (int i = 0; i < 10; i++) cc.update(backlog);
    CHECK(cc.bitrate() < 200'000'000);

    // Never below floor.
    for (int i = 0; i < 100; i++) cc.update(lossy);
    CHECK(cc.bitrate() >= c.minBitrateBps);
}

int main() {
    testFecBasics();
    testFecRecovery();
    testFecTooManyErasures();
    testPacketizerRoundTrip();
    testPacketizerFecLoss();
    testPacketizerNackAndStale();
    testPacer();
    testTimesync();
    testCongestion();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
