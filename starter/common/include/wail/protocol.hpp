#pragma once
// Wire protocol of the test task (see ../../../../protocol.md). The format is FIXED: do not change
// field order or sizes. All integers are little-endian, floats are IEEE-754 binary32 little-endian.
// Serialization is done field by field (never memcpy a whole struct: padding and endianness).
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <variant>
#include <vector>

namespace wail {

inline constexpr std::uint16_t kMagic = 0x4157;  // bytes on the wire: 'W' 'A'
inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kMaxDatagram = 1200;  // never send UDP payloads larger than this
inline constexpr std::uint16_t kDefaultServerPort = 40000;
inline constexpr std::uint16_t kDefaultTickHz = 60;
inline constexpr std::size_t kMaxPlayers = 16;

enum class MsgType : std::uint8_t {
    kHello = 1,    // client -> server: join request
    kWelcome = 2,  // server -> client: join accepted, client id assigned
    kInput = 3,    // client -> server: one input sample
    kState = 4,    // server -> all clients: world state after a tick
    kPing = 5,     // client -> server: clock / RTT probe
    kPong = 6,     // server -> client: reply to kPing
    kBye = 7,      // either direction: leave / shutdown
};

// Key bits for InputMsg::keys
enum Key : std::uint16_t {
    kKeyW = 1u << 0, kKeyA = 1u << 1, kKeyS = 1u << 2, kKeyD = 1u << 3,
    kKeySpace = 1u << 4, kKeyShift = 1u << 5, kKeyMouse1 = 1u << 6, kKeyMouse2 = 1u << 7,
};

// 16 bytes, prefix of every datagram.
struct Header {
    std::uint16_t magic = kMagic;
    std::uint8_t version = kVersion;
    MsgType type = MsgType::kHello;
    std::uint32_t seq = 0;      // per-sender, per-direction datagram counter (for loss / reorder stats)
    std::uint64_t send_ns = 0;  // sender's mono_ns() right before sendto()
};

struct HelloMsg {  // 8 bytes
    std::uint32_t nonce = 0;           // random, echoed in WelcomeMsg
    std::uint16_t requested_id = 0;    // 0 = any, otherwise ask for a specific client id (1..kMaxPlayers)
    std::uint16_t reserved = 0;
};

struct WelcomeMsg {  // 12 bytes
    std::uint32_t nonce = 0;
    std::uint16_t client_id = 0;       // 1..kMaxPlayers
    std::uint16_t tick_hz = kDefaultTickHz;
    std::uint32_t server_tick = 0;     // current server tick
};

struct InputMsg {  // 24 bytes
    std::uint16_t client_id = 0;
    std::uint16_t keys = 0;            // bitmask of Key
    std::uint32_t input_seq = 0;       // per-client, strictly increasing; identifies the input end-to-end
    std::uint32_t client_frame = 0;    // client's local frame counter (informational)
    std::int16_t mouse_dx = 0;
    std::int16_t mouse_dy = 0;
    std::uint64_t input_ns = 0;        // client's mono_ns() at the moment the input was sampled
};

struct PlayerState {  // 32 bytes
    std::uint16_t client_id = 0;
    std::uint16_t keys = 0;                 // keys of the last applied input
    std::uint32_t last_input_seq = 0;       // input_seq of the newest input of this client applied in this tick
    std::uint64_t last_input_recv_ns = 0;   // SERVER mono_ns() when that input was received
    float pos_x = 0, pos_y = 0;
    float vel_x = 0, vel_y = 0;
};

struct StateMsg {  // 16 + 32 * players.size() bytes
    std::uint32_t server_tick = 0;
    std::uint8_t player_count = 0;          // filled by encode() from players.size()
    std::uint8_t reserved[3] = {0, 0, 0};
    std::uint64_t tick_start_ns = 0;        // SERVER mono_ns() when processing of this tick started
    std::vector<PlayerState> players;       // up to kMaxPlayers
};

struct PingMsg {  // 16 bytes
    std::uint32_t ping_id = 0;
    std::uint32_t reserved = 0;
    std::uint64_t t0_ns = 0;                // client mono_ns() at send
};

struct PongMsg {  // 32 bytes
    std::uint32_t ping_id = 0;
    std::uint32_t reserved = 0;
    std::uint64_t t0_ns = 0;                // echoed from PingMsg
    std::uint64_t t1_ns = 0;                // server mono_ns() at receive
    std::uint64_t t2_ns = 0;                // server mono_ns() at send
};

struct ByeMsg {  // 4 bytes
    std::uint16_t client_id = 0;
    std::uint16_t reason = 0;               // 0 = normal
};

using Body = std::variant<HelloMsg, WelcomeMsg, InputMsg, StateMsg, PingMsg, PongMsg, ByeMsg>;

struct Packet {
    Header header;
    Body body;
};

// Serializes header + body. header.type is set from the body alternative automatically.
// Returns an empty vector if the packet would exceed kMaxDatagram or is otherwise invalid.
std::vector<std::uint8_t> encode(Header header, const Body& body);

// Parses one datagram. Returns std::nullopt for anything malformed: wrong magic/version, unknown type,
// truncated or oversized payload, player_count > kMaxPlayers, trailing bytes.
std::optional<Packet> decode(std::span<const std::uint8_t> datagram);

// Wire size of a body, without the header.
std::size_t body_size(const Body& body);

inline constexpr std::size_t kHeaderSize = 16;

}  // namespace wail
