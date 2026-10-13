#pragma once
// Built-in network profiles of the emulator (the same values as ../../profiles.yaml).
// Values are ONE-WAY: up = client -> server, down = server -> client.
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace wail {

struct LinkParams {
    double delay_ms = 0;   // mean one-way delay
    double jitter_ms = 0;  // standard deviation of the delay (normal distribution, clipped at 0)
    double loss_pct = 0;   // probability to drop a datagram, percent
};

struct ClientLink {
    LinkParams up;
    LinkParams down;
};

struct NetProfile {
    std::string name;
    std::string description;
    ClientLink dflt;                                 // used for every client without an override
    std::map<std::uint16_t, ClientLink> per_client;  // client_id -> override

    const ClientLink& link_for(std::uint16_t client_id) const {
        auto it = per_client.find(client_id);
        return it == per_client.end() ? dflt : it->second;
    }
};

inline std::vector<NetProfile> builtin_profiles() {
    std::vector<NetProfile> v;
    v.push_back({"none", "no emulation at all (raw loopback)", {{0, 0, 0}, {0, 0, 0}}, {}});
    v.push_back({"lan", "local network: ~1 ms each way, tiny jitter", {{1, 0.2, 0}, {1, 0.2, 0}}, {}});
    v.push_back({"wan_jittery", "Wi-Fi / mobile: 40 ms each way, 15 ms jitter, 1% loss for everybody",
                 {{40, 15, 1}, {40, 15, 1}}, {}});
    NetProfile asym{"asymmetric_bad_client",
                    "everybody 15 ms each way, except client 2: uplink 120+-30 ms / 3% loss, downlink 60+-10 ms / 1%",
                    {{15, 2, 0}, {15, 2, 0}},
                    {}};
    asym.per_client[2] = ClientLink{{120, 30, 3}, {60, 10, 1}};
    v.push_back(asym);
    return v;
}

}  // namespace wail
