// wail_server: READY-MADE authoritative UDP game server with a fixed tick and a built-in network emulator.
// You do NOT need to change it (but reading it is useful). The task is the client side and the measurement.
//
//   wail_server --profile asymmetric_bad_client --duration 75 --log-dir runs/asym
//   wail_server --list-profiles
//
// What it does:
//   * listens on UDP 127.0.0.1:40000 (protocol: ../../protocol.md);
//   * Hello -> Welcome (client_id = requested_id if free, otherwise the lowest free id);
//   * ticks at --tick-hz (default 60). Tick k starts at t0 + k * period (no drift). Every tick it applies
//     the NEWEST received input of every client (older inputs that arrive late are ignored), moves the
//     players (position += speed(keys) * dt) and sends State to every client;
//   * answers Ping with Pong (t1 = when the Ping reached the server logic, t2 = when the Pong left it);
//   * emulates the network PER CLIENT AND PER DIRECTION (delay, jitter, loss) according to --profile.
//     Uplink: a received datagram is held back for the sampled delay before the server logic sees it.
//     Downlink: an outgoing datagram is held back before sendto(). Jitter reorders datagrams (as tc netem).
//     All *_ns timestamps the server puts on the wire are taken at the server LOGIC, i.e. AFTER the
//     uplink emulation and BEFORE the downlink emulation (as if the network were outside the server).
//
// Exit: Ctrl+C, --duration seconds, or (with --exit-when-empty) when all clients have sent Bye.
// At exit it prints a summary (tick jitter, applied delays and losses per client, emulator accuracy)
// and, with --log-dir, writes server_ticks.csv and server_inputs.csv (see tools/log_format.md).
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#include "net_emulator.hpp"
#include "profiles.hpp"
#include "wail/clock.hpp"
#include "wail/protocol.hpp"

namespace {

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

struct Options {
    std::string bind_ip = "127.0.0.1";
    std::uint16_t port = wail::kDefaultServerPort;
    std::uint16_t tick_hz = wail::kDefaultTickHz;
    std::string profile = "lan";
    std::uint64_t seed = 1;
    double duration_s = 0;  // 0 = until Ctrl+C
    std::string log_dir;
    bool exit_when_empty = false;
    std::uint64_t spin_us = 200;  // busy-wait this long before a due event for better timer accuracy
    bool quiet = false;
};

void usage() {
    std::printf(
        "usage: wail_server [--profile NAME] [--port 40000] [--bind 127.0.0.1] [--tick-hz 60] [--seed 1]\n"
        "                   [--duration SEC] [--log-dir DIR] [--exit-when-empty] [--spin-us 200] [--quiet]\n"
        "       wail_server --list-profiles\n");
}

bool parse(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "missing value for %s\n", name);
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--profile") o.profile = next("--profile");
        else if (a == "--port") o.port = static_cast<std::uint16_t>(std::atoi(next("--port")));
        else if (a == "--bind") o.bind_ip = next("--bind");
        else if (a == "--tick-hz") o.tick_hz = static_cast<std::uint16_t>(std::atoi(next("--tick-hz")));
        else if (a == "--seed") o.seed = std::strtoull(next("--seed"), nullptr, 10);
        else if (a == "--duration") o.duration_s = std::atof(next("--duration"));
        else if (a == "--log-dir") o.log_dir = next("--log-dir");
        else if (a == "--exit-when-empty") o.exit_when_empty = true;
        else if (a == "--spin-us") o.spin_us = std::strtoull(next("--spin-us"), nullptr, 10);
        else if (a == "--quiet") o.quiet = true;
        else if (a == "--list-profiles") {
            for (const auto& p : wail::builtin_profiles()) {
                std::printf("%-22s %s\n", p.name.c_str(), p.description.c_str());
            }
            std::exit(0);
        } else {
            usage();
            return false;
        }
    }
    if (o.tick_hz == 0 || o.tick_hz > 1000) {
        std::fprintf(stderr, "--tick-hz must be 1..1000\n");
        return false;
    }
    return true;
}

bool same_addr(const sockaddr_in& a, const sockaddr_in& b) {
    return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

double percentile(std::vector<double> v, double q) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    const double pos = (v.size() - 1) * q / 100.0;
    const auto lo = static_cast<std::size_t>(pos);
    const auto hi = std::min(lo + 1, v.size() - 1);
    return v[lo] + (v[hi] - v[lo]) * (pos - static_cast<double>(lo));
}

struct Client {
    bool active = false;
    std::uint16_t id = 0;
    sockaddr_in addr{};
    std::uint32_t down_seq = 0;
    // newest input received so far (by input_seq)
    std::uint32_t last_input_seq = 0;
    std::uint64_t last_input_recv_ns = 0;
    std::uint16_t keys = 0;
    // trivial world
    float pos_x = 0, pos_y = 0, vel_x = 0, vel_y = 0;
};

struct Inbound {  // a datagram travelling client -> server through the emulator
    sockaddr_in from{};
    std::uint64_t sock_recv_ns = 0;
    wail::Packet pkt;
};

struct Outbound {  // a datagram travelling server -> client through the emulator
    sockaddr_in to{};
    std::vector<std::uint8_t> bytes;
};

struct TickLog {
    std::uint32_t tick;
    std::uint64_t scheduled_ns, tick_start_ns, send_done_ns;
    std::uint32_t players;
};

struct InputLog {
    std::uint16_t client_id;
    std::uint32_t input_seq;
    std::uint64_t input_ns, sock_recv_ns, deliver_ns;
    const char* fate;  // accepted | stale | dropped_up | unknown_client
};

class Server {
public:
    Server(const Options& o, wail::NetProfile prof) : opt_(o), prof_(std::move(prof)), sampler_(o.seed) {}

    bool open_socket() {
        fd_ = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (fd_ < 0) return perror_false("socket");
        int one = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
        int buf = 1 << 20;
        ::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &buf, sizeof buf);
        ::setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &buf, sizeof buf);
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_port = htons(opt_.port);
        if (::inet_pton(AF_INET, opt_.bind_ip.c_str(), &a.sin_addr) != 1) {
            std::fprintf(stderr, "bad --bind address %s\n", opt_.bind_ip.c_str());
            return false;
        }
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&a), sizeof a) < 0) return perror_false("bind");
        const int fl = ::fcntl(fd_, F_GETFL, 0);
        ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
        return true;
    }

    void run() {
        const std::uint64_t period_ns = 1'000'000'000ull / opt_.tick_hz;
        const std::uint64_t t0 = wail::mono_ns() + 10'000'000ull;  // first tick in 10 ms
        const std::uint64_t deadline = opt_.duration_s > 0
                                           ? t0 + static_cast<std::uint64_t>(opt_.duration_s * 1e9)
                                           : UINT64_MAX;
        auto tick_time = [&](std::uint64_t k) { return t0 + k * 1'000'000'000ull / opt_.tick_hz; };
        std::uint64_t k = 0;
        std::printf("wail_server: udp %s:%u, %u Hz, profile '%s' (%s), seed %llu\n", opt_.bind_ip.c_str(),
                    static_cast<unsigned>(opt_.port), static_cast<unsigned>(opt_.tick_hz), prof_.name.c_str(),
                    prof_.description.c_str(), static_cast<unsigned long long>(opt_.seed));
        std::fflush(stdout);

        while (!g_stop) {
            std::uint64_t now = wail::mono_ns();
            if (now >= deadline) break;
            if (opt_.exit_when_empty && had_clients_ && active_count() == 0 && out_.empty()) break;

            std::uint64_t next = std::min(tick_time(k), deadline);
            if (!in_.empty()) next = std::min(next, in_.next_due());
            if (!out_.empty()) next = std::min(next, out_.next_due());
            const std::uint64_t spin_ns = opt_.spin_us * 1000ull;
            std::uint64_t wait_ns = next > now ? next - now : 0;
            wait_ns = wait_ns > spin_ns ? wait_ns - spin_ns : 0;  // wake up a bit early, then spin
            // Sleep in short slices: the OS may add "timer slack" proportional to the timeout
            // (macOS timer coalescing), long sleeps would make ticks and emulated delays late.
            wait_ns = std::min<std::uint64_t>(wait_ns, 1'000'000ull);
            wait_readable(wait_ns);
            drain_socket();

            now = wail::mono_ns();
            std::uint64_t due = 0;
            while (auto item = in_.pop_due(now, due)) {
                record_lateness(now, due);
                handle_inbound(*item, now);
            }
            if (now >= tick_time(k)) {
                do_tick(static_cast<std::uint32_t>(k), tick_time(k));
                ++k;
                while (tick_time(k) + period_ns < wail::mono_ns()) {  // fell behind by > 1 period: skip
                    ++skipped_ticks_;
                    ++k;
                }
            }
            now = wail::mono_ns();
            while (auto item = out_.pop_due(now, due)) {
                record_lateness(now, due);
                send_raw(item->to, item->bytes);
            }
        }
        flush_outbound_now();
    }

    void report() const {
        std::vector<double> start_late;
        start_late.reserve(ticks_.size());
        for (const auto& t : ticks_) start_late.push_back((t.tick_start_ns - t.scheduled_ns) / 1e6);
        std::printf("\n=== wail_server summary ===\n");
        std::printf("ticks: %zu (skipped %llu); tick start lateness ms: p50 %.3f  p99 %.3f  max %.3f\n", ticks_.size(),
                    static_cast<unsigned long long>(skipped_ticks_), percentile(start_late, 50),
                    percentile(start_late, 99), percentile(start_late, 100));
        std::printf("datagrams: received %llu, malformed %llu, stale inputs ignored %llu, send errors %llu\n",
                    static_cast<unsigned long long>(received_), static_cast<unsigned long long>(malformed_),
                    static_cast<unsigned long long>(stale_), static_cast<unsigned long long>(send_errors_));
        std::printf("emulator dispatch lateness (actual - scheduled) ms: p50 %.3f  p99 %.3f  max %.3f\n",
                    percentile(lateness_ms_, 50), percentile(lateness_ms_, 99), percentile(lateness_ms_, 100));
        std::printf("applied one-way delays per client (profile '%s'):\n", prof_.name.c_str());
        std::printf("  client dir   passed dropped loss%%   mean_ms  p50_ms  p99_ms\n");
        for (std::size_t id = 0; id < link_stats_.size(); ++id) {
            for (int dir = 0; dir < 2; ++dir) {
                const auto& s = link_stats_[id][dir];
                if (s.passed + s.dropped == 0) continue;
                double mean = 0;
                for (double d : s.delays_ms) mean += d;
                mean = s.delays_ms.empty() ? 0 : mean / s.delays_ms.size();
                std::printf("  %6zu %-4s %7llu %7llu %5.2f %9.2f %7.2f %7.2f\n", id, dir == 0 ? "up" : "down",
                            static_cast<unsigned long long>(s.passed), static_cast<unsigned long long>(s.dropped),
                            100.0 * s.dropped / (s.passed + s.dropped), mean, percentile(s.delays_ms, 50),
                            percentile(s.delays_ms, 99));
            }
        }
    }

    bool write_logs() const {
        if (opt_.log_dir.empty()) return true;
        std::error_code ec;
        std::filesystem::create_directories(opt_.log_dir, ec);
        std::ofstream ft(opt_.log_dir + "/server_ticks.csv");
        std::ofstream fi(opt_.log_dir + "/server_inputs.csv");
        if (!ft || !fi) {
            std::fprintf(stderr, "cannot write logs into %s\n", opt_.log_dir.c_str());
            return false;
        }
        ft << "tick,scheduled_ns,tick_start_ns,send_done_ns,players\n";
        for (const auto& t : ticks_)
            ft << t.tick << ',' << t.scheduled_ns << ',' << t.tick_start_ns << ',' << t.send_done_ns << ','
               << t.players << '\n';
        fi << "client_id,input_seq,input_ns,sock_recv_ns,deliver_ns,fate\n";
        for (const auto& r : inputs_)
            fi << r.client_id << ',' << r.input_seq << ',' << r.input_ns << ',' << r.sock_recv_ns << ','
               << r.deliver_ns << ',' << r.fate << '\n';
        std::printf("logs written to %s/\n", opt_.log_dir.c_str());
        return true;
    }

private:
    static bool perror_false(const char* what) {
        std::perror(what);
        return false;
    }

    std::size_t active_count() const {
        std::size_t n = 0;
        for (const auto& c : clients_) n += c.active;
        return n;
    }

    Client* find_client(const sockaddr_in& a) {
        for (auto& c : clients_)
            if (c.active && same_addr(c.addr, a)) return &c;
        return nullptr;
    }

    // client id used to pick the emulated link for an incoming datagram
    std::uint16_t link_id_for_incoming(const sockaddr_in& from, const wail::Packet& p) {
        if (const Client* c = find_client(from)) return c->id;
        if (const auto* h = std::get_if<wail::HelloMsg>(&p.body)) return h->requested_id;
        if (const auto* in = std::get_if<wail::InputMsg>(&p.body)) return in->client_id;
        return 0;
    }

    wail::LinkStats& stats_for(std::uint16_t id, int dir) {
        if (id >= link_stats_.size()) id = 0;
        return link_stats_[id][dir];
    }

    void wait_readable(std::uint64_t wait_ns) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd_, &rs);
        timeval tv{};
        tv.tv_sec = static_cast<time_t>(wait_ns / 1'000'000'000ull);
        tv.tv_usec = static_cast<suseconds_t>((wait_ns % 1'000'000'000ull) / 1000);
        ::select(fd_ + 1, &rs, nullptr, nullptr, &tv);
    }

    void drain_socket() {
        std::array<std::uint8_t, 2048> buf{};
        for (;;) {
            sockaddr_in from{};
            socklen_t len = sizeof from;
            const ssize_t n = ::recvfrom(fd_, buf.data(), buf.size(), 0, reinterpret_cast<sockaddr*>(&from), &len);
            if (n < 0) break;  // EWOULDBLOCK / EAGAIN: nothing more to read
            const std::uint64_t t = wail::mono_ns();
            ++received_;
            auto pkt = wail::decode(std::span<const std::uint8_t>(buf.data(), static_cast<std::size_t>(n)));
            if (!pkt) {
                ++malformed_;
                continue;
            }
            const std::uint16_t lid = link_id_for_incoming(from, *pkt);
            auto delay = sampler_.sample(prof_.link_for(lid).up, stats_for(lid, 0));
            if (!delay) {
                if (const auto* in = std::get_if<wail::InputMsg>(&pkt->body))
                    inputs_.push_back({in->client_id, in->input_seq, in->input_ns, t, 0, "dropped_up"});
                continue;
            }
            in_.push(t + *delay, Inbound{from, t, std::move(*pkt)});
        }
    }

    void send_via_emulator(Client& c, wail::Header h, const wail::Body& body) {
        h.seq = ++c.down_seq;
        h.send_ns = wail::mono_ns();
        auto bytes = wail::encode(h, body);
        if (bytes.empty()) return;
        auto delay = sampler_.sample(prof_.link_for(c.id).down, stats_for(c.id, 1));
        if (!delay) return;
        out_.push(h.send_ns + *delay, Outbound{c.addr, std::move(bytes)});
    }

    void send_raw(const sockaddr_in& to, const std::vector<std::uint8_t>& bytes) {
        const ssize_t n =
            ::sendto(fd_, bytes.data(), bytes.size(), 0, reinterpret_cast<const sockaddr*>(&to), sizeof to);
        if (n != static_cast<ssize_t>(bytes.size())) ++send_errors_;
    }

    void flush_outbound_now() {
        std::uint64_t due = 0;
        while (auto item = out_.pop_due(UINT64_MAX, due)) send_raw(item->to, item->bytes);
    }

    void record_lateness(std::uint64_t now, std::uint64_t due) {
        if (lateness_ms_.size() < 2'000'000) lateness_ms_.push_back((now - due) / 1e6);
    }

    void handle_inbound(Inbound& ib, std::uint64_t now) {
        Client* c = find_client(ib.from);
        if (const auto* hello = std::get_if<wail::HelloMsg>(&ib.pkt.body)) {
            if (!c) c = admit(ib.from, hello->requested_id);
            if (!c) return;  // server full
            wail::WelcomeMsg w;
            w.nonce = hello->nonce;
            w.client_id = c->id;
            w.tick_hz = opt_.tick_hz;
            w.server_tick = last_tick_;
            send_via_emulator(*c, {}, w);
        } else if (const auto* in = std::get_if<wail::InputMsg>(&ib.pkt.body)) {
            if (!c || c->id != in->client_id) {
                inputs_.push_back({in->client_id, in->input_seq, in->input_ns, ib.sock_recv_ns, now, "unknown_client"});
                return;
            }
            if (in->input_seq > c->last_input_seq) {
                c->last_input_seq = in->input_seq;
                c->last_input_recv_ns = now;
                c->keys = in->keys;
                inputs_.push_back({c->id, in->input_seq, in->input_ns, ib.sock_recv_ns, now, "accepted"});
            } else {
                ++stale_;  // reordered: an older input arrived after a newer one
                inputs_.push_back({c->id, in->input_seq, in->input_ns, ib.sock_recv_ns, now, "stale"});
            }
        } else if (const auto* ping = std::get_if<wail::PingMsg>(&ib.pkt.body)) {
            if (!c) return;
            wail::PongMsg p;
            p.ping_id = ping->ping_id;
            p.t0_ns = ping->t0_ns;
            p.t1_ns = now;
            p.t2_ns = wail::mono_ns();
            send_via_emulator(*c, {}, p);
        } else if (std::get_if<wail::ByeMsg>(&ib.pkt.body)) {
            if (c) {
                c->active = false;
                std::printf("client %u left\n", static_cast<unsigned>(c->id));
                std::fflush(stdout);
            }
        }
    }

    Client* admit(const sockaddr_in& from, std::uint16_t requested) {
        auto taken = [&](std::uint16_t id) {
            for (const auto& c : clients_)
                if (c.active && c.id == id) return true;
            return false;
        };
        std::uint16_t id = 0;
        if (requested >= 1 && requested <= wail::kMaxPlayers && !taken(requested)) {
            id = requested;
        } else {
            for (std::uint16_t i = 1; i <= wail::kMaxPlayers; ++i)
                if (!taken(i)) {
                    id = i;
                    break;
                }
        }
        if (id == 0) return nullptr;
        Client& c = clients_[id - 1];
        c = Client{};
        c.active = true;
        c.id = id;
        c.addr = from;
        c.pos_x = -20.0f + 10.0f * static_cast<float>(id);
        had_clients_ = true;
        char ip[INET_ADDRSTRLEN] = {};
        ::inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        std::printf("client %u joined from %s:%u (link up %.0f±%.0f ms %.1f%%, down %.0f±%.0f ms %.1f%%)\n",
                    static_cast<unsigned>(id), ip, static_cast<unsigned>(ntohs(from.sin_port)),
                    prof_.link_for(id).up.delay_ms, prof_.link_for(id).up.jitter_ms, prof_.link_for(id).up.loss_pct,
                    prof_.link_for(id).down.delay_ms, prof_.link_for(id).down.jitter_ms,
                    prof_.link_for(id).down.loss_pct);
        std::fflush(stdout);
        return &c;
    }

    void do_tick(std::uint32_t tick, std::uint64_t scheduled_ns) {
        const std::uint64_t start = wail::mono_ns();
        const float dt = 1.0f / static_cast<float>(opt_.tick_hz);
        wail::StateMsg st;
        st.server_tick = tick;
        st.tick_start_ns = start;
        for (auto& c : clients_) {
            if (!c.active) continue;
            const float speed = (c.keys & wail::kKeyShift) ? 10.0f : 5.0f;  // m/s
            c.vel_x = speed * (((c.keys & wail::kKeyD) ? 1.0f : 0.0f) - ((c.keys & wail::kKeyA) ? 1.0f : 0.0f));
            c.vel_y = speed * (((c.keys & wail::kKeyW) ? 1.0f : 0.0f) - ((c.keys & wail::kKeyS) ? 1.0f : 0.0f));
            c.pos_x = std::clamp(c.pos_x + c.vel_x * dt, -100.0f, 100.0f);
            c.pos_y = std::clamp(c.pos_y + c.vel_y * dt, -100.0f, 100.0f);
            wail::PlayerState ps;
            ps.client_id = c.id;
            ps.keys = c.keys;
            ps.last_input_seq = c.last_input_seq;
            ps.last_input_recv_ns = c.last_input_recv_ns;
            ps.pos_x = c.pos_x;
            ps.pos_y = c.pos_y;
            ps.vel_x = c.vel_x;
            ps.vel_y = c.vel_y;
            st.players.push_back(ps);
        }
        for (auto& c : clients_)
            if (c.active) send_via_emulator(c, {}, st);
        last_tick_ = tick;
        ticks_.push_back({tick, scheduled_ns, start, wail::mono_ns(), static_cast<std::uint32_t>(st.players.size())});
    }

    Options opt_;
    wail::NetProfile prof_;
    wail::LinkSampler sampler_;
    int fd_ = -1;
    std::array<Client, wail::kMaxPlayers> clients_{};
    bool had_clients_ = false;
    std::uint32_t last_tick_ = 0;
    wail::DelayQueue<Inbound> in_;
    wail::DelayQueue<Outbound> out_;
    std::array<std::array<wail::LinkStats, 2>, wail::kMaxPlayers + 1> link_stats_{};  // [client_id][up/down]
    std::vector<double> lateness_ms_;
    std::vector<TickLog> ticks_;
    std::vector<InputLog> inputs_;
    std::uint64_t received_ = 0, malformed_ = 0, stale_ = 0, send_errors_ = 0, skipped_ticks_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
    Options opt;
    if (!parse(argc, argv, opt)) return 2;
    std::optional<wail::NetProfile> prof;
    for (auto& p : wail::builtin_profiles())
        if (p.name == opt.profile) prof = p;
    if (!prof) {
        std::fprintf(stderr, "unknown profile '%s' (see --list-profiles)\n", opt.profile.c_str());
        return 2;
    }
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
    Server srv(opt, *prof);
    if (!srv.open_socket()) return 1;
    srv.run();
    if (!opt.quiet) srv.report();
    return srv.write_logs() ? 0 : 1;
}
