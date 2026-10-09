#include "client_net.h"

#include <android/log.h>

#include <cstring>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, "VRStream", __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, "VRStream", __VA_ARGS__)

namespace vrstream {

namespace {

std::vector<uint8_t> buildDatagram(uint32_t streamId, PacketType type, uint32_t seq,
                                   const void* body, size_t bodyLen) {
    std::vector<uint8_t> d(sizeof(BaseHeader) + bodyLen);
    BaseHeader h{};
    h.magic = kMagic;
    h.streamId = streamId;
    h.type = static_cast<uint8_t>(type);
    h.datagramLen = static_cast<uint16_t>(d.size());
    h.sequence = seq;
    h.timestamp = nowUs();
    std::memcpy(d.data(), &h, sizeof(h));
    if (bodyLen) std::memcpy(d.data() + sizeof(h), body, bodyLen);
    return d;
}

}  // namespace

bool ClientNet::start(const FrameSink& onFrame, const PoseSource& poseSource) {
    onFrame_ = onFrame;
    poseSource_ = poseSource;

    if (!sock_.bind(0)) {
        LOGE("client socket bind failed");
        return false;
    }
    sock_.setDscpEf();

    // Try the configured host first; if it is a broadcast address, scan for
    // the first host that answers a handshake.
    if (doHandshake(cfg_.host)) {
        running_ = true;
        thread_ = std::thread([this] { netLoop(); });
        return true;
    }
    if (cfg_.host != "255.255.255.255") {
        // Fall back to broadcast discovery.
        if (doHandshake("255.255.255.255")) {
            running_ = true;
            thread_ = std::thread([this] { netLoop(); });
            return true;
        }
    }
    LOGE("no VRStream host answered on port %u", cfg_.port);
    return false;
}

bool ClientNet::doHandshake(const std::string& host) {
    sock_.enableBroadcast();
    streamId_ = static_cast<uint32_t>(nowUs() & 0x7fffffff) | 1;

    HandshakeRequestMsg req{};
    req.protocolVersion = kProtocolVersion;
    req.streamId = streamId_;
    req.codecMask = cfg_.codecMask;
    req.refreshRateHz = cfg_.refreshRateHz;
    req.eyeWidth = 1536;  // advertised capability; host decides actual size
    req.eyeHeight = 1600;
    std::snprintf(req.deviceName, sizeof(req.deviceName), "Quest 3");

    for (int attempt = 0; attempt < 3; attempt++) {
        auto dg = buildDatagram(streamId_, PacketType::HandshakeRequest, 0, &req, sizeof(req));
        sock_.sendTo(host, cfg_.port, dg.data(), dg.size());

        for (int i = 0; i < 20; i++) {  // ~2 s total
            if (!sock_.waitReadable(100 * 1000)) continue;
            uint8_t buf[2048];
            std::string from;
            uint16_t fromPort;
            size_t n = sock_.recvFrom(buf, sizeof(buf), from, fromPort);
            if (n < sizeof(BaseHeader)) continue;
            auto h = reinterpret_cast<const BaseHeader*>(buf);
            if (h->magic != kMagic || h->streamId != streamId_) continue;
            if (h->type != static_cast<uint8_t>(PacketType::HandshakeResponse)) continue;
            if (n < sizeof(BaseHeader) + sizeof(HandshakeResponseMsg)) continue;

            negotiated_ =
                *reinterpret_cast<const HandshakeResponseMsg*>(buf + sizeof(BaseHeader));
            serverAddr_ = from;
            serverPort_ = fromPort;
            LOGI("host %s:%u: %ux%u@%u codec=%u bitrate=%u fec=%u%%", from.c_str(), fromPort,
                 negotiated_.eyeWidth, negotiated_.eyeHeight, negotiated_.refreshRateHz,
                 negotiated_.codec, negotiated_.bitrateBps, negotiated_.fecPercent / 100);
            return true;
        }
    }
    return false;
}

void ClientNet::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    if (!serverAddr_.empty()) {
        sendPacket(PacketType::Disconnect, nullptr, 0);
    }
}

void ClientNet::sendPacket(PacketType type, const void* body, size_t len) {
    auto dg = buildDatagram(streamId_, type, 0, body, len);
    sock_.sendTo(serverAddr_, serverPort_, dg.data(), dg.size());
}

void ClientNet::netLoop() {
    while (running_.load()) {
        controlTick();  // rate-limited internally; cheap otherwise

        if (!sock_.waitReadable(2 * 1000)) continue;
        uint8_t buf[4096];
        std::string from;
        uint16_t fromPort;
        size_t n = sock_.recvFrom(buf, sizeof(buf), from, fromPort);
        if (n < sizeof(BaseHeader)) continue;
        packetsReceived_++;

        auto h = reinterpret_cast<const BaseHeader*>(buf);
        if (h->magic != kMagic || h->streamId != streamId_) continue;

        if (h->type == static_cast<uint8_t>(PacketType::TimeSyncResponse)) {
            if (n >= sizeof(BaseHeader) + sizeof(TimeSyncResponseMsg)) {
                auto r =
                    reinterpret_cast<const TimeSyncResponseMsg*>(buf + sizeof(BaseHeader));
                sync_.addSample({r->t1ClientUs, r->t2ServerRxUs, r->t3ServerTxUs, nowUs()});
            }
            continue;
        }
        if (h->type != static_cast<uint8_t>(PacketType::Video)) continue;
        if (n < sizeof(BaseHeader) + sizeof(VideoHeader)) continue;

        auto vh = reinterpret_cast<const VideoHeader*>(buf + sizeof(BaseHeader));
        const uint8_t* payload = buf + sizeof(BaseHeader) + sizeof(VideoHeader);
        size_t payloadLen = n - sizeof(BaseHeader) - sizeof(VideoHeader);

        if (auto frame = receiver_.ingest(*vh, payload, payloadLen)) {
            if (onFrame_) {
                onFrame_(frame->bytes.data(), frame->bytes.size(), frame->pts,
                         frame->frameIndex, frame->keyframe);
            }
        }
    }
}

void ClientNet::controlTick() {
    uint64_t now = nowUs();

    // Timesync probes: burst-friendly, keeps min-RTT offset fresh.
    static thread_local uint64_t lastSyncUs = 0;
    if (now - lastSyncUs > 250'000) {
        lastSyncUs = now;
        TimeSyncRequestMsg tr{};
        tr.t1ClientUs = now;
        sendPacket(PacketType::TimeSyncRequest, &tr, sizeof(tr));
    }

    // NACKs every 20 ms.
    static thread_local uint64_t lastNackUs = 0;
    if (now - lastNackUs > 20'000) {
        lastNackUs = now;
        auto pending = receiver_.pendingNacks(100'000);
        if (!pending.empty()) {
            NackRequestMsg nr{};
            nr.count = static_cast<uint8_t>(std::min<size_t>(pending.size(), 32));
            for (size_t i = 0; i < nr.count; i++) nr.entries[i] = pending[i];
            sendPacket(PacketType::NackRequest, &nr, sizeof(nr));
        }
    }

    // Tracking uplink at ~250 Hz when a pose source is wired.
    static thread_local uint64_t lastPoseUs = 0;
    if (poseSource_ && now - lastPoseUs > 4000) {
        lastPoseUs = now;
        PoseSample p = poseSource_();
        TrackingMsg tm{};
        tm.poseCount = 1;
        tm.head[0] = p;
        tm.head[1] = p;
        auto dg = buildDatagram(streamId_, PacketType::Tracking, seqTracking_++,
                                &tm, sizeof(tm) - sizeof(PoseSample));
        sock_.sendTo(serverAddr_, serverPort_, dg.data(), dg.size());
    }

    // Stats report every 500 ms.
    static thread_local uint64_t lastStatsUs = 0;
    if (now - lastStatsUs > 500'000) {
        lastStatsUs = now;
        StatsReportMsg sr{};
        sr.rttMs = sync_.bestRttUs() / 1000.0f;
        sr.lostPacketsTotal = 0;
        sr.fecRecoveredFrames = receiver_.fecRecoveredFrames();
        sr.fecFailedFrames = receiver_.fecFailedFrames();
        sr.decodeBacklog = 0;
        sr.decodeMs = 0;
        sr.frameAgeAtDisplayMs = 0;
        sr.goodputMbps = 0;
        sr.transportJitterMs = 0;
        sendPacket(PacketType::StatsReport, &sr, sizeof(sr));
    }
}

}  // namespace vrstream
