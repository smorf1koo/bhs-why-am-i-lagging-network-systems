// wail_client: headless test client.  THIS IS A SKELETON — writing the client is your task.
// (You may write the client in Python instead: see ../python/wail_protocol.py.)
//
// Suggested CLI (any is fine, document yours in README):
//   wail_client --server 127.0.0.1:40000 --clients 3 --input-hz 60 --duration 70 --out runs/lan
//   (one process can drive several clients — one UDP socket per client — or run one process per client)
//
// What the client has to do:
//   * join: send Hello (requested_id = 1, 2, 3, ...) and wait for Welcome; datagrams can be LOST, so
//     resend Hello every ~200 ms until Welcome arrives;
//   * every 1/input_hz seconds send Input with a NEW strictly increasing input_seq and
//     input_ns = wail::mono_ns() taken right when the input is "pressed"; change keys regularly;
//   * receive State; States can arrive out of order (jitter) or be lost: ignore a State whose
//     server_tick is older than the newest one you have seen;
//   * for every OTHER client A in the State: when players[A].last_input_seq grows, all of A's inputs
//     up to that seq became visible to this client B at this moment -> log it;
//   * remember your own inputs (input_seq -> input_ns);
//   * at the end send Bye and write logs; a separate script (or this program) joins A's input log
//     with B's "seen" log into one CSV (tools/log_format.md) and computes percentiles.
//
// Clock: decide how A's input_ns and B's receive time become comparable and justify it in report.md.
#include <cstdio>

#include "wail/clock.hpp"
#include "wail/protocol.hpp"

int main(int argc, char** argv) {
    (void)argc;
    (void)argv;
    std::printf("wail_client: not implemented yet (now = %llu ns)\n",
                static_cast<unsigned long long>(wail::mono_ns()));
    // TODO: your client
    return 0;
}
