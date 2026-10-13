// Reference implementation of the fixed wire format (protocol.md). Provided ready-made: the task is
// about the networking and the measurement methodology, not about byte packing.
#include "wail/protocol.hpp"

#include <bit>
#include <cstring>
#include <type_traits>

namespace wail {

namespace {

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}
    void u8(std::uint8_t v) { out_.push_back(v); }
    void u16(std::uint16_t v) { le(v); }
    void u32(std::uint32_t v) { le(v); }
    void u64(std::uint64_t v) { le(v); }
    void i16(std::int16_t v) { le(static_cast<std::uint16_t>(v)); }
    void f32(float v) { le(std::bit_cast<std::uint32_t>(v)); }

private:
    template <class T>
    void le(T v) {
        static_assert(std::is_unsigned_v<T>);
        for (std::size_t i = 0; i < sizeof(T); ++i) out_.push_back(static_cast<std::uint8_t>(v >> (8 * i)));
    }
    std::vector<std::uint8_t>& out_;
};

class Reader {
public:
    explicit Reader(std::span<const std::uint8_t> in) : in_(in) {}
    bool ok() const { return ok_; }
    bool at_end() const { return pos_ == in_.size(); }
    std::size_t remaining() const { return in_.size() - pos_; }
    std::uint8_t u8() { return le<std::uint8_t>(); }
    std::uint16_t u16() { return le<std::uint16_t>(); }
    std::uint32_t u32() { return le<std::uint32_t>(); }
    std::uint64_t u64() { return le<std::uint64_t>(); }
    std::int16_t i16() { return static_cast<std::int16_t>(le<std::uint16_t>()); }
    float f32() { return std::bit_cast<float>(le<std::uint32_t>()); }

private:
    template <class T>
    T le() {
        if (!ok_ || remaining() < sizeof(T)) {
            ok_ = false;
            return T{};
        }
        T v = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) v = static_cast<T>(v | (static_cast<T>(in_[pos_ + i]) << (8 * i)));
        pos_ += sizeof(T);
        return v;
    }
    std::span<const std::uint8_t> in_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

template <class... Ts>
struct Overloaded : Ts... {
    using Ts::operator()...;
};
template <class... Ts>
Overloaded(Ts...) -> Overloaded<Ts...>;

MsgType type_of(const Body& b) {
    return std::visit(Overloaded{
                          [](const HelloMsg&) { return MsgType::kHello; },
                          [](const WelcomeMsg&) { return MsgType::kWelcome; },
                          [](const InputMsg&) { return MsgType::kInput; },
                          [](const StateMsg&) { return MsgType::kState; },
                          [](const PingMsg&) { return MsgType::kPing; },
                          [](const PongMsg&) { return MsgType::kPong; },
                          [](const ByeMsg&) { return MsgType::kBye; },
                      },
                      b);
}

}  // namespace

std::size_t body_size(const Body& body) {
    return std::visit(Overloaded{
                          [](const HelloMsg&) -> std::size_t { return 8; },
                          [](const WelcomeMsg&) -> std::size_t { return 12; },
                          [](const InputMsg&) -> std::size_t { return 24; },
                          [](const StateMsg& s) -> std::size_t { return 16 + 32 * s.players.size(); },
                          [](const PingMsg&) -> std::size_t { return 16; },
                          [](const PongMsg&) -> std::size_t { return 32; },
                          [](const ByeMsg&) -> std::size_t { return 4; },
                      },
                      body);
}

std::vector<std::uint8_t> encode(Header h, const Body& body) {
    std::vector<std::uint8_t> out;
    if (const auto* s = std::get_if<StateMsg>(&body); s && s->players.size() > kMaxPlayers) return out;
    const std::size_t total = kHeaderSize + body_size(body);
    if (total > kMaxDatagram) return out;
    out.reserve(total);
    Writer w(out);
    h.type = type_of(body);
    w.u16(h.magic);
    w.u8(h.version);
    w.u8(static_cast<std::uint8_t>(h.type));
    w.u32(h.seq);
    w.u64(h.send_ns);
    std::visit(Overloaded{
                   [&](const HelloMsg& m) { w.u32(m.nonce); w.u16(m.requested_id); w.u16(m.reserved); },
                   [&](const WelcomeMsg& m) { w.u32(m.nonce); w.u16(m.client_id); w.u16(m.tick_hz); w.u32(m.server_tick); },
                   [&](const InputMsg& m) {
                       w.u16(m.client_id); w.u16(m.keys); w.u32(m.input_seq); w.u32(m.client_frame);
                       w.i16(m.mouse_dx); w.i16(m.mouse_dy); w.u64(m.input_ns);
                   },
                   [&](const StateMsg& m) {
                       w.u32(m.server_tick);
                       w.u8(static_cast<std::uint8_t>(m.players.size()));
                       w.u8(0); w.u8(0); w.u8(0);
                       w.u64(m.tick_start_ns);
                       for (const auto& p : m.players) {
                           w.u16(p.client_id); w.u16(p.keys); w.u32(p.last_input_seq); w.u64(p.last_input_recv_ns);
                           w.f32(p.pos_x); w.f32(p.pos_y); w.f32(p.vel_x); w.f32(p.vel_y);
                       }
                   },
                   [&](const PingMsg& m) { w.u32(m.ping_id); w.u32(m.reserved); w.u64(m.t0_ns); },
                   [&](const PongMsg& m) { w.u32(m.ping_id); w.u32(m.reserved); w.u64(m.t0_ns); w.u64(m.t1_ns); w.u64(m.t2_ns); },
                   [&](const ByeMsg& m) { w.u16(m.client_id); w.u16(m.reason); },
               },
               body);
    return out;
}

std::optional<Packet> decode(std::span<const std::uint8_t> data) {
    if (data.size() < kHeaderSize || data.size() > kMaxDatagram) return std::nullopt;
    Reader r(data);
    Packet p;
    p.header.magic = r.u16();
    p.header.version = r.u8();
    const std::uint8_t type = r.u8();
    p.header.seq = r.u32();
    p.header.send_ns = r.u64();
    if (p.header.magic != kMagic || p.header.version != kVersion) return std::nullopt;
    p.header.type = static_cast<MsgType>(type);
    switch (p.header.type) {
        case MsgType::kHello: {
            HelloMsg m;
            m.nonce = r.u32(); m.requested_id = r.u16(); m.reserved = r.u16();
            p.body = m;
            break;
        }
        case MsgType::kWelcome: {
            WelcomeMsg m;
            m.nonce = r.u32(); m.client_id = r.u16(); m.tick_hz = r.u16(); m.server_tick = r.u32();
            p.body = m;
            break;
        }
        case MsgType::kInput: {
            InputMsg m;
            m.client_id = r.u16(); m.keys = r.u16(); m.input_seq = r.u32(); m.client_frame = r.u32();
            m.mouse_dx = r.i16(); m.mouse_dy = r.i16(); m.input_ns = r.u64();
            p.body = m;
            break;
        }
        case MsgType::kState: {
            StateMsg m;
            m.server_tick = r.u32();
            m.player_count = r.u8();
            m.reserved[0] = r.u8(); m.reserved[1] = r.u8(); m.reserved[2] = r.u8();
            m.tick_start_ns = r.u64();
            if (m.player_count > kMaxPlayers) return std::nullopt;
            m.players.resize(m.player_count);
            for (auto& ps : m.players) {
                ps.client_id = r.u16(); ps.keys = r.u16(); ps.last_input_seq = r.u32(); ps.last_input_recv_ns = r.u64();
                ps.pos_x = r.f32(); ps.pos_y = r.f32(); ps.vel_x = r.f32(); ps.vel_y = r.f32();
            }
            p.body = std::move(m);
            break;
        }
        case MsgType::kPing: {
            PingMsg m;
            m.ping_id = r.u32(); m.reserved = r.u32(); m.t0_ns = r.u64();
            p.body = m;
            break;
        }
        case MsgType::kPong: {
            PongMsg m;
            m.ping_id = r.u32(); m.reserved = r.u32(); m.t0_ns = r.u64(); m.t1_ns = r.u64(); m.t2_ns = r.u64();
            p.body = m;
            break;
        }
        case MsgType::kBye: {
            ByeMsg m;
            m.client_id = r.u16(); m.reason = r.u16();
            p.body = m;
            break;
        }
        default:
            return std::nullopt;
    }
    if (!r.ok() || !r.at_end()) return std::nullopt;
    return p;
}

}  // namespace wail
