// VRStream host entry point.
#include <cstdio>
#include <cstring>
#include <string>

#include "session.h"

static void usage() {
    std::printf(
        "vrstream_host — low-latency PC VR streaming host\n"
        "\n"
        "Usage: vrstream_host [options]\n"
        "  --port N          UDP port (default 9944)\n"
        "  --fps N           target framerate (default 90)\n"
        "  --width/--height  encoded frame size (default 2560x1440 stereo pair)\n"
        "  --codec C         h264 | hevc | av1 (default h264)\n"
        "  --bitrate MBPS    target bitrate in Mbps (default 150)\n"
        "  --fec PCT         Reed-Solomon overhead percent (default 8)\n"
        "  --mtu BYTES       UDP payload per packet (default 1200)\n"
        "  --duration SEC    stop after SEC seconds (0 = until client disconnects)\n"
        "  --self-test       run an in-process loopback client and exit\n"
        "  --no-encode       transport test with deterministic payloads (no GPU needed)\n"
        "  --feed-port N     receive driver-encoded frames on loopback N (default 9955 with SteamVR driver)\n"
        "  --test-loss PCT   packet drop percent injected in self-test (default 0)\n"
        "  --test-frames N   self-test frame count (default 300)\n"
        "  --test-out FILE   self-test raw bitstream output (default out.h264)\n");
}

int main(int argc, char** argv) {
    setvbuf(stdout, nullptr, _IONBF, 0);
    vrstream::HostConfig cfg;
    int durationSec = 0;

    for (int i = 1; i < argc; i++) {
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                exit(2);
            }
            return argv[++i];
        };
        if (!strcmp(argv[i], "--port")) cfg.port = static_cast<uint16_t>(atoi(next("--port")));
        else if (!strcmp(argv[i], "--fps")) cfg.fps = static_cast<uint32_t>(atoi(next("--fps")));
        else if (!strcmp(argv[i], "--width"))
            cfg.width = static_cast<uint32_t>(atoi(next("--width")));
        else if (!strcmp(argv[i], "--height"))
            cfg.height = static_cast<uint32_t>(atoi(next("--height")));
        else if (!strcmp(argv[i], "--codec")) {
            const char* c = next("--codec");
            if (!strcmp(c, "h264")) cfg.codec = vrstream::Codec::H264;
            else if (!strcmp(c, "hevc")) cfg.codec = vrstream::Codec::H265;
            else if (!strcmp(c, "hevc10")) cfg.codec = vrstream::Codec::H26510;
            else if (!strcmp(c, "av1")) cfg.codec = vrstream::Codec::Av1;
            else { std::fprintf(stderr, "unknown codec %s\n", c); return 2; }
        } else if (!strcmp(argv[i], "--bitrate"))
            cfg.bitrateBps = static_cast<uint32_t>(atof(next("--bitrate")) * 1e6);
        else if (!strcmp(argv[i], "--fec")) cfg.fecPercent = atof(next("--fec"));
        else if (!strcmp(argv[i], "--mtu")) cfg.mtu = static_cast<uint16_t>(atoi(next("--mtu")));
        else if (!strcmp(argv[i], "--duration")) durationSec = atoi(next("--duration"));
        else if (!strcmp(argv[i], "--self-test")) cfg.selfTest = true;
        else if (!strcmp(argv[i], "--no-encode")) cfg.noEncode = true;
        else if (!strcmp(argv[i], "--feed-port"))
            cfg.feedPort = static_cast<uint16_t>(atoi(next("--feed-port")));
        else if (!strcmp(argv[i], "--test-loss")) cfg.selfTestLossPct = atof(next("--test-loss"));
        else if (!strcmp(argv[i], "--test-frames"))
            cfg.selfTestFrames = atoi(next("--test-frames"));
        else if (!strcmp(argv[i], "--test-out")) cfg.selfTestOut = next("--test-out");
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "unknown option %s\n", argv[i]);
            usage();
            return 2;
        }
    }

    vrstream::HostSession session(cfg);
    return session.run(durationSec);
}
