// Minimal non-blocking UDP socket, portable across Windows and Android/Linux.
#pragma once

#include <cstdint>
#include <string>

namespace vrstream {

class UdpSocket {
  public:
    UdpSocket() = default;
    ~UdpSocket();

    UdpSocket(const UdpSocket&) = delete;
    UdpSocket& operator=(const UdpSocket&) = delete;

    // Binds to `port` (0 = ephemeral). Returns false on failure.
    bool bind(uint16_t port);

    // Marks the socket for sending broadcasts (needed for discovery).
    bool enableBroadcast();

    // Best-effort QoS marking (DSCP EF). Ignored if the OS refuses.
    void setDscpEf();

    // Sends one datagram to addr:port. Returns false on failure.
    bool sendTo(const std::string& addr, uint16_t port, const uint8_t* data, size_t len);

    // Receives one datagram if one is pending (non-blocking).
    // Returns the number of bytes written into `buf`, or 0 if none pending.
    size_t recvFrom(uint8_t* buf, size_t cap, std::string& fromAddr, uint16_t& fromPort);

    // Waits up to `timeoutUs` for a readable datagram.
    bool waitReadable(uint64_t timeoutUs);

    uint16_t localPort() const { return localPort_; }

  private:
    int64_t sock_ = -1;  // SOCKET on Windows is UINT_PTR; int64 holds it
    uint16_t localPort_ = 0;
};

}  // namespace vrstream
