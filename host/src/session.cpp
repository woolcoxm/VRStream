#include "session.h"

#include <windows.h>

#pragma comment(lib, "winmm.lib")

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <random>

#include "d3d_helpers.h"
#include "test_source.h"

namespace vrstream {

namespace {

std::vector<uint8_t> buildDatagram(uint32_t streamId, PacketType type, uint32_t seq,
                                   const void* body, size_t bodyLen) {
    std::vector<uint8_t> d(sizeof(BaseHeader) + bodyLen);
    BaseHeader h{};
    h.magic = kMagic;
    h.streamId = streamId;
    h.type = static_cast<uint8_t>(type);
    h.reserved0 = 0;
    h.datagramLen = static_cast<uint16_t>(d.size());
    h.sequence = seq;
    h.timestamp = nowUs();
    std::memcpy(d.data(), &h, sizeof(h));
    if (bodyLen) std::memcpy(d.data() + sizeof(h), body, bodyLen);
    return d;
}

const BaseHeader* asBase(const uint8_t* buf, size_t len) {
    if (len < sizeof(BaseHeader)) return nullptr;
    auto h = reinterpret_cast<const BaseHeader*>(buf);
    if (h->magic != kMagic) return nullptr;
    return h;
}

}  // namespace

HostSession::HostSession(HostConfig cfg)
    : cfg_(std::move(cfg)),
      packetizer_(FrameSenderConfig{cfg_.mtu, cfg_.fecPercent, 60}),
      congestion_(CongestionController::Config{20'000'000, 400'000'000, cfg_.bitrateBps / 2}) {
    fecPercent_.store(cfg_.fecPercent);
}

HostSession::~HostSession() {
    running_ = false;
    sendCv_.notify_all();
}

int HostSession::run(int durationSec) {
    timeBeginPeriod(1);  // 1 ms sleep granularity for the pacer
    struct PeriodGuard {
        ~PeriodGuard() { timeEndPeriod(1); }
    } periodGuard;

    if (!sock_.bind(cfg_.port)) {
        std::fprintf(stderr, "failed to bind UDP %u\n", cfg_.port);
        return 1;
    }
    sock_.setDscpEf();
    std::printf("VRStream host listening on UDP %u (mtu=%u fps=%u %ux%u %s @ %.0f Mbps)\n",
                cfg_.port, cfg_.mtu, cfg_.fps, cfg_.width, cfg_.height,
                codecName(cfg_.codec), cfg_.bitrateBps / 1e6);

    if (cfg_.selfTest) {
        std::thread([this] { runSelfTestClient(); }).detach();
    }

    // Wait for a handshake.
    uint64_t deadline = nowUs() + (durationSec > 0 ? uint64_t(durationSec) * 1000000ull
                                                   : 60ull * 60 * 1000000);
    while (!streaming_ && nowUs() < deadline) {
        uint8_t buf[2048];
        std::string from;
        uint16_t fromPort;
        sock_.waitReadable(200 * 1000);
        size_t n = sock_.recvFrom(buf, sizeof(buf), from, fromPort);
        if (n) handleReceived(buf, n, from, fromPort);
    }
    if (!streaming_) {
        std::printf("no client connected\n");
        return 1;
    }

    // Encoder init on the first NVENC-capable adapter.
    auto adapters = enumerateD3dAdapters();
    bool encoderReady = cfg_.noEncode;
    if (cfg_.noEncode) std::printf("no-encode mode: deterministic payloads (transport test)\n");
    for (auto& a : adapters) {
        if (encoderReady) break;
        std::printf("adapter: %s (%zu MB) -- probing NVENC\n", a.name.c_str(),
                    a.dedicatedVramMb);
        NvencEncoder::Caps caps;
        if (NvencEncoder::probe(a.device.Get(), &caps)) {
            std::printf("encoding on: %s (h264=%d hevc=%d av1=%d)\n", a.name.c_str(),
                        caps.h264, caps.h265, caps.av1);
            Codec use = cfg_.codec;
            if (use == Codec::Av1 && !caps.av1) {
                std::printf("AV1 unsupported on this GPU, falling back to H.264\n");
                use = Codec::H264;
            }
            if (!encoder_.init(a.device.Get(), use, cfg_.width, cfg_.height, cfg_.fps,
                               cfg_.bitrateBps)) {
                std::fprintf(stderr, "encoder init failed on %s\n", a.name.c_str());
                continue;
            }
            encoderReady = true;
            break;
        }
    }
    if (!encoderReady) {
        std::fprintf(stderr, "no NVENC-capable adapter found\n");
        return 1;
    }

    running_ = true;
    if (cfg_.feedPort) {
        if (!feedSock_.bind(cfg_.feedPort)) {
            std::fprintf(stderr, "failed to bind feed port %u\n", cfg_.feedPort);
            running_ = false;
        } else {
            std::printf("receiving driver frames on loopback UDP %u\n", cfg_.feedPort);
            feedThread_ = std::thread([this] { feedLoop(); });
        }
    }
    std::thread encodeTh([this] { encodeLoop(); });
    std::thread sendTh([this] { sendLoop(); });
    std::thread recvTh([this] { recvLoop(); });
    std::thread controlTh([this] { controlLoop(); });

    const uint64_t startedUs = nowUs();
    uint64_t limitUs = durationSec > 0 ? startedUs + uint64_t(durationSec) * 1000000ull
                                       : UINT64_MAX;
    while (running_ && nowUs() < limitUs) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::lock_guard<std::mutex> lk(stats_.rxMx_);
        if (streaming_ && stats_.lastReportAtUs &&
            nowUs() - stats_.lastReportAtUs > 3'000'000) {
            std::printf("client went silent, ending session\n");
            break;
        }
        if (!streaming_) break;
    }
    running_ = false;
    sendCv_.notify_all();
    feedCv_.notify_all();
    encodeTh.join();
    sendTh.join();
    recvTh.join();
    controlTh.join();
    if (feedThread_.joinable()) feedThread_.join();

    StatsReportMsg rep;
    {
        std::lock_guard<std::mutex> lk(stats_.rxMx_);
        rep = stats_.lastReport;
    }
    std::printf(
        "session done: %u frames encoded, %.1f MB sent, retx %llu\n",
        stats_.encodedFrames.load(), stats_.sentBytes.load() / 1e6,
        static_cast<unsigned long long>(stats_.retxPackets.load()));
    return 0;
}

void HostSession::handleReceived(const uint8_t* buf, size_t len, const std::string& from,
                                 uint16_t fromPort) {
    const BaseHeader* h = asBase(buf, len);
    if (!h) return;
    if (streamId_.load() && h->streamId != streamId_.load() &&
        h->type != static_cast<uint8_t>(PacketType::HandshakeRequest))
        return;

    switch (static_cast<PacketType>(h->type)) {
        case PacketType::HandshakeRequest: {
            if (len < sizeof(BaseHeader) + sizeof(HandshakeRequestMsg)) return;
            auto req = reinterpret_cast<const HandshakeRequestMsg*>(buf + sizeof(BaseHeader));
            streamId_ = req->streamId ? req->streamId : (uint32_t)(nowUs() & 0x7fffffff);
            clientAddr_ = from;
            clientPort_ = fromPort;

            HandshakeResponseMsg resp{};
            resp.protocolVersion = kProtocolVersion;
            resp.streamId = streamId_;
            resp.codec = static_cast<uint16_t>(cfg_.codec);
            resp.refreshRateHz = static_cast<uint16_t>(cfg_.fps);
            resp.eyeWidth = static_cast<uint16_t>(cfg_.width);
            resp.eyeHeight = static_cast<uint16_t>(cfg_.height);
            resp.bitrateBps = cfg_.bitrateBps;
            resp.fecPercent = static_cast<uint16_t>(cfg_.fecPercent * 100);
            resp.mtu = cfg_.mtu;
            sendControlTo(from, fromPort, PacketType::HandshakeResponse, &resp, sizeof(resp));
            std::printf("client %s:%u connected: %s (%ux%u@%u, codecs=0x%x)\n",
                        from.c_str(), fromPort, req->deviceName, req->eyeWidth,
                        req->eyeHeight, req->refreshRateHz, req->codecMask);
            streaming_ = true;
            break;
        }
        case PacketType::TimeSyncRequest: {
            if (len < sizeof(BaseHeader) + sizeof(TimeSyncRequestMsg)) return;
            TimeSyncResponseMsg resp{};
            resp.t1ClientUs =
                reinterpret_cast<const TimeSyncRequestMsg*>(buf + sizeof(BaseHeader))
                    ->t1ClientUs;
            resp.t2ServerRxUs = nowUs();
            resp.t3ServerTxUs = nowUs();
            sendControlTo(from, fromPort, PacketType::TimeSyncResponse, &resp, sizeof(resp));
            break;
        }
        case PacketType::StatsReport: {
            if (len < sizeof(BaseHeader) + sizeof(StatsReportMsg)) return;
            std::lock_guard<std::mutex> lk(stats_.rxMx_);
            stats_.lastReport =
                *reinterpret_cast<const StatsReportMsg*>(buf + sizeof(BaseHeader));
            stats_.lastReportAtUs = nowUs();
            break;
        }
        case PacketType::NackRequest: {
            if (len < sizeof(BaseHeader) + 1) return;
            serviceNacks(*reinterpret_cast<const NackRequestMsg*>(buf + sizeof(BaseHeader)),
                         from, fromPort);
            break;
        }
        case PacketType::KeyframeRequest:
            keyframeRequested_ = true;
            break;
        case PacketType::Disconnect:
            std::printf("client disconnected\n");
            streaming_ = false;
            running_ = false;
            break;
        default:
            break;
    }
}

void HostSession::serviceNacks(const NackRequestMsg& nack, const std::string& addr,
                               uint16_t port) {
    std::lock_guard<std::mutex> lk(ringMx_);
    for (int i = 0; i < nack.count && i < 32; i++) {
        const NackEntry& e = nack.entries[i];
        auto fit = retransmitRing_.find(e.frameIndex);
        if (fit == retransmitRing_.end()) continue;
        RingKey key{e.groupIdx, e.packetIdx, 0};
        auto kit = fit->second.find(key);
        if (kit == fit->second.end()) continue;
        sock_.sendTo(addr, port, kit->second.data(), kit->second.size());
        stats_.retxPackets++;
    }
}

void HostSession::feedLoop() {
    uint8_t buf[2048];
    while (running_) {
        if (!feedSock_.waitReadable(100 * 1000)) continue;
        std::string from;
        uint16_t fromPort;
        size_t n = feedSock_.recvFrom(buf, sizeof(buf), from, fromPort);
        if (n < 4) continue;
        uint32_t magic = 0;
        std::memcpy(&magic, buf, 4);

        // Per-frame render poses arrive as a separate small datagram.
        if (magic == kFeedMetaMagic) {
            if (n < sizeof(FeedMetaMsg)) continue;
            auto m = reinterpret_cast<const FeedMetaMsg*>(buf);
            std::lock_guard<std::mutex> lk(feedMetaMx_);
            feedMeta_[m->frameCounter] = m->meta;
            if (feedMeta_.size() > 16) feedMeta_.erase(feedMeta_.begin());
            continue;
        }
        if (magic != kFeedMagic) continue;
        auto h = reinterpret_cast<const FeedPacketHeader*>(buf);
        if (h->fragIdx >= h->fragCount || h->fragCount == 0 || h->fragCount > 512) continue;

        auto& frags = feedFrags_[h->frameCounter];
        if (frags.empty()) frags.resize(h->fragCount);
        size_t payloadLen = n - sizeof(FeedPacketHeader);
        if (frags[h->fragIdx].empty())
            frags[h->fragIdx].assign(buf + sizeof(FeedPacketHeader),
                                     buf + sizeof(FeedPacketHeader) + payloadLen);
        feedPts_[h->frameCounter] = h->ptsUs;

        bool all = true;
        for (auto& f : frags)
            if (f.empty()) all = false;
        if (!all) continue;

        FedFrame ff;
        ff.ptsUs = feedPts_[h->frameCounter];
        {
            std::lock_guard<std::mutex> lk(feedMetaMx_);
            auto mit = feedMeta_.find(h->frameCounter);
            if (mit != feedMeta_.end()) {
                ff.hasMeta = true;
                ff.meta = mit->second;
                feedMeta_.erase(mit);
            }
        }
        for (auto& f : frags) ff.annexB.insert(ff.annexB.end(), f.begin(), f.end());

        // Drop stale frame counters to bound memory.
        if (feedFrags_.size() > 8) {
            feedFrags_.erase(feedFrags_.begin());
            feedPts_.erase(feedPts_.begin());
        }
        feedFrags_.erase(h->frameCounter);
        feedPts_.erase(h->frameCounter);

        std::lock_guard<std::mutex> lk(feedMx_);
        while (feedQueue_.size() >= 3) feedQueue_.pop_front();  // newest-wins
        feedQueue_.push_back(std::move(ff));
        feedCv_.notify_one();
    }
}

void HostSession::encodeLoop() {
    TestSource source(cfg_.width, cfg_.height);
    const auto period = std::chrono::microseconds(1000000 / cfg_.fps);
    auto next = std::chrono::steady_clock::now();
    uint64_t n = 0;

    while (running_) {
        if (!cfg_.feedPort) next += period;
        uint64_t pts = nowUs();
        n++;

        bool idr = false;
        if (n == 1) {
            idr = true;
        } else if (keyframeRequested_.exchange(false)) {
            if (nowUs() - lastIdrUs_ > 100'000) {  // rate-limit IDRs
                idr = true;
                std::printf("keyframe requested -> IDR\n");
            }
        } else if (!cfg_.feedPort && n % (cfg_.fps * 10) == 0) {
            idr = true;  // periodic refresh safety net
        }
        if (idr) lastIdrUs_ = nowUs();

        std::vector<uint8_t> annexB;
        bool hasMeta = false;
        VideoMetaMsg meta{};
        if (cfg_.feedPort) {
            // SteamVR drives the cadence; wait for the driver's frame.
            std::unique_lock<std::mutex> lk(feedMx_);
            feedCv_.wait_for(lk, std::chrono::milliseconds(100),
                             [this] { return !feedQueue_.empty() || !running_.load(); });
            if (feedQueue_.empty()) continue;
            FedFrame latest = std::move(feedQueue_.back());
            feedQueue_.clear();
            annexB = std::move(latest.annexB);
            pts = latest.ptsUs;
            hasMeta = latest.hasMeta;
            meta = latest.meta;
            // Keyframe decision comes from the driver's frame 0 flag; the
            // bitstream already contains the IDR, flag only affects pacing
            // metadata on our side.
            idr = false;
            lk.unlock();
            n++;
        }

        const uint8_t* rgba = (!cfg_.feedPort && !cfg_.noEncode) ? source.render(n) : nullptr;
        uint64_t t0 = nowUs();
        uint32_t fi = frameIndex_.fetch_add(1);
        if (cfg_.feedPort) {
            // Driver already encoded the frame; annexB/pts came from the feed.
        } else if (cfg_.noEncode) {
            // Deterministic checksummed payload sized like a real encoded
            // frame at the target bitrate (keyframes ~3x). Layout:
            //   [0..4) magic | [4..8) frameIndex | [8..16) FNV-1a64(rest)
            uint32_t effectiveBps = encoder_.ok() ? encoder_.bitrate() : congestion_.bitrate();
            double target = std::max(1u, effectiveBps) / 8.0 / cfg_.fps;
            if (idr) target *= 3.0;
            // A little per-frame variation so packet counts differ.
            std::mt19937 rng(static_cast<uint32_t>(n * 2654435761u));
            size_t sz = std::max<size_t>(64, static_cast<size_t>(target * (0.85 + 0.3 * (rng() % 1000) / 1000.0)));
            annexB.assign(sz, 0);
            auto* p = reinterpret_cast<uint32_t*>(annexB.data());
            p[0] = 0x56525331u;  // 'VRS1'
            p[1] = fi;
            std::mt19937 body(static_cast<uint32_t>(fi * 40503u + 7));
            for (size_t i = 16; i < sz; i++) annexB[i] = static_cast<uint8_t>(body());
            uint64_t h = 0xcbf29ce484222325ull;
            for (size_t i = 16; i < sz; i++) {
                h ^= annexB[i];
                h *= 0x100000001b3ull;
            }
            std::memcpy(annexB.data() + 8, &h, 8);
        } else if (!encoder_.encode(rgba, source.rowPitch(), idr, pts, annexB)) {
            std::fprintf(stderr, "encode failed, stopping\n");
            running_ = false;
            break;
        }
        uint64_t t1 = nowUs();
        stats_.encodedFrames++;
        stats_.encodeTimeUsTotal += t1 - t0;
        stats_.encodeBytesTotal += annexB.size();

        packetizer_.setFecPercent(fecPercent_.load());
        auto dgrams = packetizer_.packetize(annexB.data(), annexB.size(), fi, pts, idr);
        const uint64_t readyUs = nowUs();
        if (hasMeta) {
            // Render poses ride ahead of the video packets (timewarp
            // contract); loss degrades to client-local poses, never a stall.
            meta.frameIndex = fi;
            meta.pts = pts;
            std::lock_guard<std::mutex> lk(sendMx_);
            sendQueue_.push_back(
                {readyUs, buildDatagram(streamId_.load(), PacketType::VideoMeta,
                                        stats_.sentPackets.load(), &meta, sizeof(meta))});
        }
        queueVideoDatagrams(dgrams, fi, readyUs);

        if (!cfg_.feedPort) std::this_thread::sleep_until(next);
    }
}

void HostSession::queueVideoDatagrams(std::vector<PacketizedDatagram>& dgrams,
                                      uint32_t frameIndex, uint64_t frameReadyUs) {
    // Pacing anchors to when the frame became ready (post-encode), not its
    // capture timestamp: a slow encode must not turn every deadline into the
    // past and degenerate the pacer into a burst.
    const uint64_t interval = 1000000 / cfg_.fps;
    const uint64_t window = interval * 6 / 10;  // spread over 60% of the frame
    const uint64_t per = window / (dgrams.size() ? dgrams.size() : 1);

    // Build all wire datagrams once.
    std::vector<std::vector<uint8_t>> wire(dgrams.size());
    for (size_t i = 0; i < dgrams.size(); i++) {
        auto& d = dgrams[i];
        std::vector<uint8_t> dg(sizeof(BaseHeader) + sizeof(VideoHeader) + d.payload.size());
        BaseHeader h{};
        h.magic = kMagic;
        h.streamId = streamId_.load();
        h.type = static_cast<uint8_t>(PacketType::Video);
        h.datagramLen = static_cast<uint16_t>(dg.size());
        h.sequence = static_cast<uint32_t>(stats_.sentPackets.load() + i);
        h.timestamp = nowUs();
        std::memcpy(dg.data(), &h, sizeof(h));
        std::memcpy(dg.data() + sizeof(h), &d.header, sizeof(VideoHeader));
        std::memcpy(dg.data() + sizeof(h) + sizeof(VideoHeader), d.payload.data(),
                    d.payload.size());
        wire[i] = std::move(dg);
    }

    // Retransmit ring (data packets only, raw datagrams).
    {
        std::lock_guard<std::mutex> lk(ringMx_);
        auto& ring = retransmitRing_[frameIndex];
        if (retransmitRing_.size() > 64) retransmitRing_.erase(retransmitRing_.begin());
        for (size_t i = 0; i < dgrams.size(); i++) {
            if (dgrams[i].header.isFec) continue;
            RingKey key{dgrams[i].header.groupIdx, dgrams[i].header.packetInGroup, 0};
            ring[key] = wire[i];
        }
    }

    std::lock_guard<std::mutex> lk(sendMx_);
    uint64_t at = frameReadyUs;
    for (auto& dg : wire) {
        sendQueue_.push_back({at, std::move(dg)});
        stats_.sentPackets++;
        at += per;
    }
    sendCv_.notify_one();
}

void HostSession::sendLoop() {
    while (running_) {
        std::unique_lock<std::mutex> lk(sendMx_);
        if (sendQueue_.empty()) {
            sendCv_.wait_for(lk, std::chrono::milliseconds(5));
            continue;
        }
        uint64_t earliest = sendQueue_.front().sendAtUs;
        uint64_t now = nowUs();
        if (now < earliest) {
            uint64_t waitUs = earliest - now;
            // Coarse sleep for >2 ms, spin the rest (Windows timer jitter).
            if (waitUs > 2000) {
                sendCv_.wait_for(lk, std::chrono::microseconds(waitUs - 1500));
            } else {
                lk.unlock();
                std::this_thread::yield();
                lk.lock();
                continue;
            }
            continue;
        }
        PacedSend item = std::move(sendQueue_.front());
        sendQueue_.pop_front();
        lk.unlock();
        if (streaming_) {
            sock_.sendTo(clientAddr_, clientPort_, item.datagram.data(),
                         item.datagram.size());
            stats_.sentBytes += item.datagram.size();
        }
    }
}

void HostSession::recvLoop() {
    uint8_t buf[4096];
    while (running_) {
        if (!sock_.waitReadable(50 * 1000)) continue;
        std::string from;
        uint16_t fromPort;
        size_t n = sock_.recvFrom(buf, sizeof(buf), from, fromPort);
        if (n) handleReceived(buf, n, from, fromPort);
    }
}

void HostSession::controlLoop() {
    uint32_t lastLossPkts = 0;
    uint32_t lastEncoded = 0;
    uint64_t lastSentPkts = 0;
    uint64_t lastPrintUs = nowUs();
    uint64_t lastAbrUs = nowUs();

    while (running_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        uint64_t now = nowUs();

        if (now - lastAbrUs >= 500'000 && streaming_) {
            lastAbrUs = now;
            StatsReportMsg rep;
            {
                std::lock_guard<std::mutex> lk(stats_.rxMx_);
                rep = stats_.lastReport;
            }
            CongestionInputs in;
            uint32_t lossDelta = rep.lostPacketsTotal >= lastLossPkts
                                     ? rep.lostPacketsTotal - lastLossPkts
                                     : 0;
            lastLossPkts = rep.lostPacketsTotal;
            uint64_t sentDelta = stats_.sentPackets.load() - lastSentPkts;
            lastSentPkts = stats_.sentPackets.load();
            in.lossPercent =
                sentDelta > 0 ? 100.f * lossDelta / static_cast<float>(sentDelta) : 0.f;
            in.goodputMbps = rep.goodputMbps;
            in.decodeBacklog = rep.decodeBacklog;
            in.frameAgeAtDisplayMs = rep.frameAgeAtDisplayMs;
            uint32_t bps = congestion_.update(in);
            encoder_.setBitrate(bps);
            // FEC follows measured loss (4%..20%), research: adaptive 16-20%
            // absorbs WiFi bursts without wasting 10x the loss rate.
            fecPercent_.store(clampv(4.0 + in.lossPercent * 2.0, 4.0, 20.0));
        }

        if (now - lastPrintUs >= 1'000'000) {
            lastPrintUs = now;
            StatsReportMsg rep;
            {
                std::lock_guard<std::mutex> lk(stats_.rxMx_);
                rep = stats_.lastReport;
            }
            uint32_t enc = stats_.encodedFrames.load();
            uint32_t fps = enc - std::exchange(lastEncoded, enc);
            double encMs = enc ? (stats_.encodeTimeUsTotal.load() / 1000.0 / enc) : 0;
            double frameKB = enc ? stats_.encodeBytesTotal.load() / 1024.0 / enc : 0;
            uint32_t activeBr = encoder_.ok() ? encoder_.bitrate() : congestion_.bitrate();
            std::printf(
                "%3u fps | enc %4.1fms %6.1fKB/f | br %4.0fM (tgt %4.0fM) | rtt "
                "%4.1fms backlog %u dec %4.1fms age %5.1fms | loss %u fec ok/short "
                "%u/%u retx %llu | pkts %llu\n",
                fps, encMs, frameKB, activeBr / 1e6,
                congestion_.bitrate() / 1e6, rep.rttMs, rep.decodeBacklog, rep.decodeMs,
                rep.frameAgeAtDisplayMs, rep.lostPacketsTotal, rep.fecRecoveredFrames,
                rep.fecFailedFrames, static_cast<unsigned long long>(stats_.retxPackets.load()),
                static_cast<unsigned long long>(stats_.sentPackets.load()));
        }
    }
}

void HostSession::sendControlTo(const std::string& addr, uint16_t port, PacketType type,
                                const void* body, size_t bodyLen) {
    auto dg = buildDatagram(streamId_.load(), type, 0, body, bodyLen);
    sock_.sendTo(addr, port, dg.data(), dg.size());
}

// ---------------------------------------------------------------------------
// Self-test client: loopback receiver in the same process.
// ---------------------------------------------------------------------------

void HostSession::runSelfTestClient() {
    UdpSocket cli;
    if (!cli.bind(0)) return;
    const uint16_t hostPort = cfg_.port;

    std::mt19937 rng(4242);
    std::bernoulli_distribution drop(cfg_.selfTestLossPct / 100.0);

    FrameReceiver receiver;
    ClockSync sync;
    uint32_t streamId = static_cast<uint32_t>(nowUs() & 0x7fffffff) | 1;

    // Handshake.
    HandshakeRequestMsg req{};
    req.protocolVersion = kProtocolVersion;
    req.streamId = streamId;
    req.codecMask = 0b1111;
    req.refreshRateHz = static_cast<uint16_t>(cfg_.fps);
    req.eyeWidth = static_cast<uint16_t>(cfg_.width);
    req.eyeHeight = static_cast<uint16_t>(cfg_.height);
    std::snprintf(req.deviceName, sizeof(req.deviceName), "self-test");
    {
        auto dg = buildDatagram(streamId, PacketType::HandshakeRequest, 0, &req, sizeof(req));
        cli.sendTo("127.0.0.1", hostPort, dg.data(), dg.size());
    }

    std::ofstream out(cfg_.selfTestOut, std::ios::binary);
    uint64_t firstRecvUs = 0, lastRecvUs = 0;
    uint64_t transportUsSum = 0, reassemblyUsSum = 0;
    uint32_t framesGot = 0, packetsGot = 0, packetsDropped = 0, corruptFrames = 0;
    uint64_t lastNackUs = 0, lastSyncUs = 0, lastStatsUs = 0;
    std::vector<uint64_t> firstSeen;

    auto tEnd = nowUs() + uint64_t(cfg_.selfTestFrames + 150) * (1000000 / cfg_.fps) +
                2'000'000;
    while (nowUs() < tEnd && framesGot < (uint32_t)cfg_.selfTestFrames) {
        // Timesync probes.
        if (nowUs() - lastSyncUs > 50'000) {
            lastSyncUs = nowUs();
            TimeSyncRequestMsg tr{};
            tr.t1ClientUs = nowUs();
            auto dg = buildDatagram(streamId, PacketType::TimeSyncRequest, 0, &tr, sizeof(tr));
            cli.sendTo("127.0.0.1", hostPort, dg.data(), dg.size());
        }
        // Stats reports.
        if (streaming_ && nowUs() - lastStatsUs > 500'000) {
            lastStatsUs = nowUs();
            StatsReportMsg sr{};
            sr.rttMs = sync.bestRttUs() / 1000.0f;
            sr.decodeBacklog = 1;
            sr.decodeMs = 3.5f;
            sr.frameAgeAtDisplayMs = 18.0f;
            sr.lostPacketsTotal = packetsDropped;
            sr.fecRecoveredFrames = receiver.fecRecoveredFrames();
            sr.fecFailedFrames = receiver.fecFailedFrames();
            auto dg = buildDatagram(streamId, PacketType::StatsReport, 0, &sr, sizeof(sr));
            cli.sendTo("127.0.0.1", hostPort, dg.data(), dg.size());
        }
        // NACKs.
        if (streaming_ && nowUs() - lastNackUs > 20'000) {
            lastNackUs = nowUs();
            auto pending = receiver.pendingNacks(100'000);
            if (!pending.empty()) {
                NackRequestMsg nr{};
                nr.count = static_cast<uint8_t>(std::min<size_t>(pending.size(), 32));
                for (size_t i = 0; i < nr.count; i++) nr.entries[i] = pending[i];
                auto dg =
                    buildDatagram(streamId, PacketType::NackRequest, 0, &nr, sizeof(nr));
                cli.sendTo("127.0.0.1", hostPort, dg.data(), dg.size());
            }
        }

        if (!cli.waitReadable(1000)) continue;
        uint8_t buf[4096];
        std::string from;
        uint16_t fromPort;
        size_t n = cli.recvFrom(buf, sizeof(buf), from, fromPort);
        if (!n) continue;
        const BaseHeader* h = asBase(buf, n);
        if (!h || h->streamId != streamId) continue;

        if (h->type == static_cast<uint8_t>(PacketType::TimeSyncResponse)) {
            if (n >= sizeof(BaseHeader) + sizeof(TimeSyncResponseMsg)) {
                auto r = reinterpret_cast<const TimeSyncResponseMsg*>(buf + sizeof(BaseHeader));
                sync.addSample({r->t1ClientUs, r->t2ServerRxUs, r->t3ServerTxUs, nowUs()});
            }
            continue;
        }
        if (h->type != static_cast<uint8_t>(PacketType::Video)) continue;
        if (n < sizeof(BaseHeader) + sizeof(VideoHeader)) continue;

        packetsGot++;
        if (drop(rng)) {
            packetsDropped++;
            continue;
        }
        auto vh = reinterpret_cast<const VideoHeader*>(buf + sizeof(BaseHeader));
        const uint8_t* payload = buf + sizeof(BaseHeader) + sizeof(VideoHeader);
        size_t payloadLen = n - sizeof(BaseHeader) - sizeof(VideoHeader);

        uint64_t now = nowUs();
        if (firstSeen.size() <= vh->frameIndex) firstSeen.resize(vh->frameIndex + 1, 0);
        if (!firstSeen[vh->frameIndex]) firstSeen[vh->frameIndex] = now;

        if (auto frame = receiver.ingest(*vh, payload, payloadLen)) {
            framesGot++;
            // Verify the deterministic payload (transport integrity check);
            // real encoder output is validated externally with ffprobe.
            if (cfg_.noEncode) {
                bool valid = frame->bytes.size() >= 16;
                if (valid) {
                    uint32_t magic = 0, frameNo = 0;
                    uint64_t storedHash = 0;
                    std::memcpy(&magic, frame->bytes.data(), 4);
                    std::memcpy(&frameNo, frame->bytes.data() + 4, 4);
                    std::memcpy(&storedHash, frame->bytes.data() + 8, 8);
                    uint64_t fnv = 0xcbf29ce484222325ull;
                    for (size_t i = 16; i < frame->bytes.size(); i++) {
                        fnv ^= frame->bytes[i];
                        fnv *= 0x100000001b3ull;
                    }
                    valid = magic == 0x56525331u && storedHash == fnv &&
                            frameNo == frame->frameIndex;
                }
                if (!valid) corruptFrames++;
            }
            out.write(reinterpret_cast<const char*>(frame->bytes.data()), frame->bytes.size());
            int64_t offset = 0;
            if (sync.offsetUs(offset)) {
                uint64_t firstHostUs = firstSeen[frame->frameIndex] + offset;
                transportUsSum += firstHostUs > frame->pts ? firstHostUs - frame->pts : 0;
            }
            reassemblyUsSum += now - firstSeen[frame->frameIndex];
        }
        lastRecvUs = now;
    }
    (void)firstRecvUs;

    std::printf(
        "\nself-test: %u/%d frames (%u corrupt), %u packets (%u dropped = %.2f%%), fec "
        "recovered %u, fec failed %u, stale-dropped frames %u\n",
        framesGot, cfg_.selfTestFrames, corruptFrames, packetsGot, packetsDropped,
        packetsGot ? 100.0 * packetsDropped / (packetsGot + packetsDropped) : 0.0,
        receiver.fecRecoveredFrames(), receiver.fecFailedFrames(),
        receiver.staleDroppedFrames());
    if (framesGot) {
        std::printf(
            "self-test: avg pts->first-packet %.2f ms (host clock), avg first->complete "
            "%.2f ms, out=%s\n",
            transportUsSum / 1000.0 / framesGot, reassemblyUsSum / 1000.0 / framesGot,
            cfg_.selfTestOut.c_str());
    }
    // Tell the host to stop.
    auto dg = buildDatagram(streamId, PacketType::Disconnect, 0, nullptr, 0);
    cli.sendTo("127.0.0.1", hostPort, dg.data(), dg.size());
    streaming_ = false;
    running_ = false;
}

}  // namespace vrstream
