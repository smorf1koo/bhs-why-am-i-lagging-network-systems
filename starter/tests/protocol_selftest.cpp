// Round-trip and golden-bytes test of the wire format. Must keep passing.
#include <cstdio>
#include <cstdlib>

#include "wail/protocol.hpp"

using namespace wail;

static int failures = 0;
#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);     \
            ++failures;                                                  \
        }                                                                \
    } while (0)

int main() {
    // golden bytes of an InputMsg (see protocol.md, "Example")
    Header h;
    h.seq = 7;
    h.send_ns = 0x0102030405060708ull;
    InputMsg in;
    in.client_id = 2;
    in.keys = kKeyW | kKeyMouse1;
    in.input_seq = 1000;
    in.client_frame = 55;
    in.mouse_dx = -3;
    in.mouse_dy = 4;
    in.input_ns = 0x1122334455667788ull;
    const auto bytes = encode(h, in);
    const std::uint8_t golden[] = {
        0x57, 0x41, 0x01, 0x03, 0x07, 0x00, 0x00, 0x00, 0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01,  // header
        0x02, 0x00, 0x41, 0x00, 0xE8, 0x03, 0x00, 0x00, 0x37, 0x00, 0x00, 0x00, 0xFD, 0xFF, 0x04, 0x00,  // input
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11};
    CHECK(bytes.size() == sizeof(golden));
    for (std::size_t i = 0; i < sizeof(golden) && i < bytes.size(); ++i) CHECK(bytes[i] == golden[i]);

    const auto back = decode(bytes);
    CHECK(back.has_value());
    if (back) {
        CHECK(back->header.type == MsgType::kInput);
        const auto* m = std::get_if<InputMsg>(&back->body);
        CHECK(m && m->input_seq == 1000 && m->mouse_dx == -3 && m->input_ns == in.input_ns);
    }

    // state round trip with 16 players
    StateMsg st;
    st.server_tick = 123456;
    st.tick_start_ns = 42;
    for (std::uint16_t i = 1; i <= kMaxPlayers; ++i) {
        PlayerState ps;
        ps.client_id = i;
        ps.last_input_seq = 10u * i;
        ps.last_input_recv_ns = 1000u * i;
        ps.pos_x = 1.5f * i;
        ps.vel_y = -0.25f;
        st.players.push_back(ps);
    }
    const auto sb = encode(Header{}, st);
    CHECK(sb.size() == kHeaderSize + 16 + 32 * kMaxPlayers);
    const auto sp = decode(sb);
    CHECK(sp.has_value());
    if (sp) {
        const auto* m = std::get_if<StateMsg>(&sp->body);
        CHECK(m && m->players.size() == kMaxPlayers && m->players[15].last_input_seq == 160 && m->players[3].pos_x == 6.0f);
    }

    // malformed input is rejected
    auto bad = bytes;
    bad[0] = 0;
    CHECK(!decode(bad).has_value());
    auto truncated = bytes;
    truncated.pop_back();
    CHECK(!decode(truncated).has_value());
    auto trailing = bytes;
    trailing.push_back(0);
    CHECK(!decode(trailing).has_value());
    st.players.push_back({});  // 17 players
    CHECK(encode(Header{}, st).empty());

    std::printf(failures ? "protocol_selftest: %d failure(s)\n" : "protocol_selftest: OK\n", failures);
    return failures ? EXIT_FAILURE : EXIT_SUCCESS;
}
