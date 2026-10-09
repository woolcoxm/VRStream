#include "vrstream/packetizer.h"

#include <cmath>

namespace vrstream {

std::vector<PacketizedDatagram> FrameSender::packetize(const uint8_t* frame,
                                                       size_t frameBytes,
                                                       uint32_t frameIndex, uint64_t pts,
                                                       bool keyframe) {
    std::vector<PacketizedDatagram> out;
    if (frameBytes == 0) return out;

    const size_t p = cfg_.mtu;
    size_t totalData = (frameBytes + p - 1) / p;

    // Repair packets per group, bounded so data + repair <= 255.
    size_t groupSize = std::min<size_t>(cfg_.maxGroupSize, totalData);
    size_t repairPerGroup =
        static_cast<size_t>(std::lround(groupSize * cfg_.fecPercent / 100.0));
    if (groupSize + repairPerGroup > Fec::kMaxSymbols)
        groupSize = Fec::kMaxSymbols - repairPerGroup;
    size_t groupCount = (totalData + groupSize - 1) / groupSize;

    size_t globalIdx = 0;
    for (size_t g = 0; g < groupCount; g++) {
        size_t count = std::min(groupSize, totalData - globalIdx);
        size_t repair = repairPerGroup;
        if (groupCount > 1 && g == groupCount - 1) {
            // Keep protection proportional on the short tail group.
            repair = static_cast<size_t>(std::lround(count * cfg_.fecPercent / 100.0));
        }

        std::vector<std::vector<uint8_t>> data(count);
        for (size_t i = 0; i < count; i++) {
            size_t off = (globalIdx + i) * p;
            size_t len = std::min(p, frameBytes - off);
            data[i].assign(frame + off, frame + off + len);
            data[i].resize(p, 0);  // equal lengths required by column-wise RS

            VideoHeader hdr{};
            hdr.frameIndex = frameIndex;
            hdr.groupIdx = static_cast<uint16_t>(g);
            hdr.dataCount = static_cast<uint16_t>(count);
            hdr.fecCount = static_cast<uint16_t>(repair);
            hdr.isFec = 0;
            hdr.flags = keyframe ? kVideoKeyframe : 0;
            hdr.packetInGroup = static_cast<uint16_t>(i);
            hdr.payloadOffset = static_cast<uint32_t>(off);
            hdr.frameBytes = static_cast<uint32_t>(frameBytes);
            hdr.pts = pts;
            out.push_back({hdr, data[i]});
        }

        auto repairs = Fec::encode(data, repair);
        for (size_t j = 0; j < repairs.size(); j++) {
            VideoHeader hdr{};
            hdr.frameIndex = frameIndex;
            hdr.groupIdx = static_cast<uint16_t>(g);
            hdr.dataCount = static_cast<uint16_t>(count);
            hdr.fecCount = static_cast<uint16_t>(repair);
            hdr.isFec = 1;
            hdr.flags = keyframe ? kVideoKeyframe : 0;
            hdr.packetInGroup = static_cast<uint16_t>(j);
            hdr.payloadOffset = 0;
            hdr.frameBytes = static_cast<uint32_t>(frameBytes);
            hdr.pts = pts;
            out.push_back({hdr, std::move(repairs[j])});
        }
        globalIdx += count;
    }
    return out;
}

std::optional<ReceivedFrame> FrameReceiver::ingest(const VideoHeader& hdr,
                                                   const uint8_t* payload,
                                                   size_t payloadLen) {
    if (haveDelivered_ && hdr.frameIndex <= lastDelivered_) {
        staleDropped_++;
        return std::nullopt;  // late retransmit for an already-passed frame
    }

    // Bound memory when frames arrive badly out of order.
    while (frames_.size() > 64)
        frames_.erase(frames_.begin());

    auto& fs = frames_[hdr.frameIndex];
    fs.frameIndex = hdr.frameIndex;
    fs.pts = hdr.pts;
    fs.flags |= hdr.flags;
    fs.frameBytes = std::max(fs.frameBytes, hdr.frameBytes);
    if (packetLen_ == 0) packetLen_ = payloadLen;

    auto& grp = fs.group[hdr.groupIdx];
    bool fresh = grp.received == 0;
    if (fresh) {
        grp.firstRxUs = nowUs();
        grp.dataCount = hdr.dataCount;
        grp.fecCount = hdr.fecCount;
        grp.data.assign(hdr.dataCount, {});
        grp.repairs.assign(hdr.fecCount, {});
    } else if (grp.dataCount != hdr.dataCount || grp.fecCount != hdr.fecCount) {
        return std::nullopt;  // corrupt
    }

    auto& slot = hdr.isFec ? grp.repairs[hdr.packetInGroup] : grp.data[hdr.packetInGroup];
    if (!slot.empty()) return std::nullopt;  // duplicate
    slot.assign(payload, payload + payloadLen);
    grp.received++;

    if (tryComplete(fs)) {
        ReceivedFrame rf;
        rf.frameIndex = fs.frameIndex;
        rf.pts = fs.pts;
        rf.keyframe = (fs.flags & kVideoKeyframe) != 0;
        for (auto& [gi, g] : fs.group)
            for (auto& pkt : g.data) rf.bytes.insert(rf.bytes.end(), pkt.begin(), pkt.end());
        rf.bytes.resize(fs.frameBytes);

        lastDelivered_ = fs.frameIndex;
        haveDelivered_ = true;
        for (auto it = frames_.begin(); it != frames_.end();) {
            if (it->first < lastDelivered_) {
                staleDropped_++;
                it = frames_.erase(it);
            } else if (it->first == lastDelivered_) {
                it = frames_.erase(it);  // the delivered frame itself
            } else {
                ++it;
            }
        }
        return rf;
    }
    return std::nullopt;
}

bool FrameReceiver::tryComplete(FrameState& fs) {
    bool complete = true;
    for (auto& [gi, g] : fs.group) {
        size_t have = 0;
        for (auto& d : g.data)
            if (!d.empty()) have++;
        if (have < g.dataCount) {
            tryFec(g);
            have = 0;
            for (auto& d : g.data)
                if (!d.empty()) have++;
            if (have < g.dataCount) complete = false;
        }
    }
    if (!complete) return false;

    // Every group also needs to have been announced (groups arrive only with
    // packets); a frame whose full group set has not been seen cannot be
    // assembled. We detect missing groups by total data packet count.
    size_t expectedData = 0;
    for (auto& [gi, g] : fs.group) expectedData += g.dataCount;
    return expectedData * packetLen_ >= fs.frameBytes;
}

void FrameReceiver::tryFec(GroupState& g) {
    size_t missing = 0;
    for (auto& d : g.data)
        if (d.empty()) missing++;
    size_t haveRepairs = 0;
    for (auto& r : g.repairs)
        if (!r.empty()) haveRepairs++;
    if (missing == 0 || missing > haveRepairs) {
        if (missing > g.fecCount) fecFailed_++;
        return;
    }

    std::vector<const std::vector<uint8_t>*> data(g.dataCount, nullptr);
    for (size_t i = 0; i < g.dataCount; i++)
        if (!g.data[i].empty()) data[i] = &g.data[i];
    std::vector<const std::vector<uint8_t>*> repairs(g.fecCount, nullptr);
    for (size_t j = 0; j < g.fecCount; j++)
        if (!g.repairs[j].empty()) repairs[j] = &g.repairs[j];

    std::vector<std::vector<uint8_t>> recovered;
    if (!Fec::decode(data, repairs, packetLen_, recovered)) {
        fecFailed_++;
        return;
    }
    bool any = false;
    for (size_t i = 0; i < g.dataCount; i++) {
        if (g.data[i].empty() && !recovered[i].empty()) {
            g.data[i] = std::move(recovered[i]);
            any = true;
        }
    }
    if (any) fecRecovered_++;
}

std::vector<NackEntry> FrameReceiver::pendingNacks(uint64_t /*nowUs*/,
                                                   uint64_t staleAfterUs) const {
    std::vector<NackEntry> nacks;
    for (auto& [fi, fs] : frames_) {
        if (haveDelivered_ && fi <= lastDelivered_) continue;
        if (nowUs() - fs.group.begin()->second.firstRxUs > staleAfterUs) continue;
        for (auto& [gi, g] : fs.group) {
            size_t missing = 0;
            for (auto& d : g.data)
                if (d.empty()) missing++;
            size_t haveRepairs = 0;
            for (auto& r : g.repairs)
                if (!r.empty()) haveRepairs++;
            // Only ask for retransmission when FEC cannot save the group.
            if (missing > haveRepairs) {
                for (size_t i = 0; i < g.dataCount; i++) {
                    if (g.data[i].empty()) {
                        NackEntry e{};
                        e.frameIndex = fi;
                        e.groupIdx = gi;
                        e.packetIdx = static_cast<uint16_t>(i);
                        nacks.push_back(e);
                    }
                }
            }
        }
    }
    return nacks;
}

}  // namespace vrstream
