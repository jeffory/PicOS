"""The firmware network stack's close, timeout and abort paths
(specs/test-audit-2026-09-24.md §3.6 and R13).

Runs only against a simulator built with SIM_FIRMWARE_NET=ON:

    make simulator-net            # build_sim_net/ (or simulator-net-asan/-tsan)
    PICOS_SIM_BINARY=build_sim_net/picos_simulator \\
        SDL_VIDEODRIVER=dummy pytest tests/e2e/test_network_firmware.py -v

That build runs the firmware's src/drivers/wifi.c, http.c and tcp.c on
Mongoose/POSIX with Core 0 and Core 1 as two host threads (simulator/net/),
so the Core 0/Core 1 lifetime bugs of the code review reproduce here. Against
any other build every test skips (allow-listed through the firmware_net
marker).

Each case runs in its own simulator: the app tests/e2e/net_fw/main.lua runs
the one case named in /data/<APP_ID>/servers.json, next to the ports of the
local servers (net_servers.py). A case that crashes the simulator therefore
fails alone, with the crash or sanitizer report as its evidence.

Cases that fail because of a known review bug are strict xfails: the fix
turns them into XPASS, which fails the run until the marker is removed.
"""

from __future__ import annotations

import json
import time
from pathlib import Path

import pytest

from helpers import E2E_DIR, case_params, lua_case_names, new_simulator, \
    run_lua_app, stage_lua_app, known_bug
from net_servers import (BlackholeServer, HttpTestServer, TcpEchoServer,
                         big_body)

pytestmark = pytest.mark.firmware_net

APP = "net_fw"
APP_ID = "com.test.net_fw"
APP_SRC = E2E_DIR / "net_fw" / "main.lua"

# The Lua-side cases, one pytest id each. Two are halves of a Python-driven
# scenario (test_http_inflight_across_app_exit) and one is the app's own
# "unknown case" guard; tcp_leave_open is driven by
# test_tcp_app_exit_closes_sockets and http_read_timeout_hang by
# test_read_timeout_fires_before_headers.
_DRIVEN = {"http_leave_inflight", "http_after_inflight_exit",
           "tcp_leave_open", "http_read_timeout_hang", "unknown_case"}
CASES = [n for n in lua_case_names(APP_SRC) if n not in _DRIVEN]

# Every file:line below is against develop after d49ff75 (the firmware
# http.c and wifi.c HTTP paths are unchanged since); re-derive them if the
# code moves.
HTTP_TIMEOUT_UAF = ("review: HTTP timeout-path use-after-free — every "
                    "http_check_timeouts branch (connect http.c:766-774, read "
                    "777-785, transfer 787-795) sets is_closing and drops pcb "
                    "but keeps nc->fn_data; the bridge frees the slot on "
                    "FAILED (lua_bridge_network.c:140) and MG_EV_CLOSE then "
                    "runs pending_set on the zeroed slot (http.c:447 -> 45)")
HTTP_READ_TIMEOUT_ARMING = (
    "new: the read timeout is armed but never checked before the headers — "
    "MG_EV_CONNECT sets deadline_read with the state SENDING (http.c:195-197), "
    "http_check_timeouts reads deadline_read only in HEADERS/BODY "
    "(http.c:777) and HTTP_STATE_HEADERS is dead (never assigned), so a "
    "server that accepts and never answers hangs the request forever")

KNOWN_BUGS = {
    "http_pool_exhaustion":
        "review: each HTTP/TCP object claims its own hardware spinlock "
        "(spin_lock_claim_unused(true), http.c:509, tcp.c:41) — only ids "
        "24-31 are claimable, so the 7th live object panics instead of the "
        "9th failing cleanly",
    "http_connect_timeout": HTTP_TIMEOUT_UAF,
    "http_read_timeout_mid_body": HTTP_TIMEOUT_UAF,
    "http_callbacks_not_reentrant":
        "new: HTTP callbacks re-enter — http_lua_fire_pending is called with "
        "the instruction hook live from sys.sleep (lua_bridge_sys.c:62-63) "
        "and the terminal's wait loop (lua_bridge_terminal.c:253-254), and "
        "the hook (lua_bridge.c:142-143) calls it again from inside the "
        "running callback; tcp_lua_fire_pending sits at the same three "
        "sites, so TCP callbacks will re-enter the same way once it "
        "dispatches them",
    "http_keepalive_reuse":
        "review: keep-alive reuse always fails — the reuse path "
        "(wifi.c:393-399) sends the second request on the connection whose "
        "HTTP handler the first response detached (http.c:284), so the "
        "response is never parsed",
    "http_chunked_body":
        "review: the headers handler's mg_iobuf_del detaches Mongoose's "
        "HTTP handler, so chunk-size lines leak into the body and COMPLETE "
        "never fires (http.c:284)",
    "http_close_delimited_body":
        "review: COMPLETE never fires for a close-delimited response "
        "(http.c:284 detach; MG_EV_CLOSE reports CLOSED, http.c:443-447)",
    "http_post_binary":
        "review: the POST body is copied with http_strdup (strlen, "
        "http.c:25-29; called at http.c:652) and sent with "
        "memcpy(tx_len) (http.c:141): truncated at the first NUL and a heap "
        "over-read",
}


@pytest.fixture
def servers():
    http = HttpTestServer().start()
    echo = TcpEchoServer().start()
    hole = BlackholeServer().start()
    assert hole.is_black(), "black-hole port completed a handshake"
    yield {"http": http, "echo": echo, "blackhole": hole}
    http.stop()
    echo.stop()
    hole.stop()


_BIG_SUM = None


def _adler(data: bytes) -> int:
    a, b = 1, 0
    for byte in data:
        a = (a + byte) % 65521
        b = (b + a) % 65521
    return b * 65536 + a


def _big_sum() -> int:
    global _BIG_SUM
    if _BIG_SUM is None:
        _BIG_SUM = _adler(big_body())
    return _BIG_SUM


def stage(sim, servers, case: str):
    """Stage the app and point it at `case` and the servers."""
    sd = Path(sim.sd_card_path)
    stage_lua_app(sd, APP, APP_SRC.read_text(), requirements=["http"],
                  id=APP_ID)
    data = sd / "data" / APP_ID
    data.mkdir(parents=True, exist_ok=True)
    (data / "servers.json").write_text(json.dumps({
        "http": servers["http"].port,
        "echo": servers["echo"].port,
        "blackhole": servers["blackhole"].port,
        "big_sum": _big_sum(),
        "case": case,
    }))


def run_case(sim, servers, case: str, timeout: float = 40.0):
    stage(sim, servers, case)
    return run_lua_app(sim, APP, timeout=timeout)


def test_firmware_stack_is_up(simulator):
    """The firmware stack came up: wifi.c joined the stand-in network and
    its connectivity check (to the shim's loopback listener) went ONLINE."""
    deadline = time.monotonic() + 10
    state = simulator.call("get_wifi_state")
    while state["status"] != "online" and time.monotonic() < deadline:
        time.sleep(0.1)
        state = simulator.call("get_wifi_state")
    assert state["status"] == "online", state
    assert state["ip"] == "127.0.0.1", state
    assert state["ssid"] == "SimulatorWiFi", state


@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_case(servers, simulator, case):
    run = run_case(simulator, servers, case)
    run.check_case(case)
    assert run.outcome.get("result") == "returned", run.describe()


@known_bug(KNOWN_BUGS["http_post_binary"])
def test_post_binary_server_side(servers, simulator):
    """The bytes the server received for the binary POST (the Lua case
    checks the echo; this pins what went on the wire)."""
    run = run_case(simulator, servers, "http_post_binary")
    payload = b"a\0b\0c" + b"\0\1\2\3" * 64 + b"end"
    assert servers["http"].posts, "no POST reached the server\n" + run.describe()
    got = servers["http"].posts[0]
    assert got == payload, (
        f"server got {len(got)} of {len(payload)} bytes: {got[:16]!r}")


@known_bug(HTTP_READ_TIMEOUT_ARMING)
def test_read_timeout_fires_before_headers(request, servers, simulator_binary,
                                           test_sd_card, tmp_path):
    """setReadTimeout(1) on a server that accepts and never answers: the
    firmware must give up and hang up within ~1 s. Judged on the server side
    only (/hang records when the client closes), on a simulator this test
    starts itself so the health hook ignores it: once the timeout fires,
    today's timeout-path use-after-free (HTTP_TIMEOUT_UAF, owned by
    http_read_timeout_mid_body) crashes the simulator right after, and the
    arming fix must still flip this test to XPASS on its own."""
    sim = new_simulator(request.config, simulator_binary, test_sd_card,
                        tmp_path / "crash_unwatched.log")
    try:
        stage(sim, servers, "http_read_timeout_hang")
        sim.launch_app(APP)
        deadline = time.monotonic() + 6
        while not servers["http"].hang_closed and time.monotonic() < deadline:
            time.sleep(0.05)
    finally:
        sim.stop()
    closed = servers["http"].hang_closed
    assert closed and closed[0] < 2.5, (
        f"client did not hang up on /hang within 2.5 s of setReadTimeout(1) "
        f"(server saw {closed})")


def test_tcp_close_releases_server_side(servers, simulator):
    """sock:close() must close the connection: the echo server sees EOF."""
    run = run_case(simulator, servers, "tcp_echo")
    run.check_case("tcp_echo")
    still_open = servers["echo"].wait_all_closed(timeout=2.0)
    assert still_open == 0, (
        f"{still_open} TCP connection(s) still open 2 s after sock:close() "
        f"(accepted {servers['echo'].accepted})")


def test_tcp_app_exit_closes_sockets(servers, simulator):
    """An app that exits with sockets open must not leave them open:
    teardown GCs the sockets (tcp_free)."""
    run = run_case(simulator, servers, "tcp_leave_open")
    run.check_case("tcp_leave_open")
    assert servers["echo"].accepted == 3
    still_open = servers["echo"].wait_all_closed(timeout=2.0)
    assert still_open == 0, (
        f"{still_open} of 3 TCP connection(s) still open 2 s after the app "
        f"exited")


def test_http_inflight_across_app_exit(servers, simulator):
    """§3.6 cross-app: exit with requests in flight, relaunch, and new
    requests work (the next launch's lua_bridge_register runs
    http_close_all; the exit itself GCs the connections)."""
    first = run_case(simulator, servers, "http_leave_inflight")
    first.check_case("http_leave_inflight")
    second = run_case(simulator, servers, "http_after_inflight_exit")
    second.check_case("http_after_inflight_exit")


def test_open_close_churn_leaves_heap_flat(servers, simulator):
    """20 request lifecycles on recycled slots: no crash, and the second
    run of the loop costs no more heap than the first (umm/Lua bytes)."""
    run_case(simulator, servers, "http_open_close_loop").check_case(
        "http_open_close_loop")
    free_1 = simulator.call("get_heap_info")["lua_heap_free_kb"]
    run_case(simulator, servers, "http_open_close_loop").check_case(
        "http_open_close_loop")
    free_2 = simulator.call("get_heap_info")["lua_heap_free_kb"]
    assert free_1 - free_2 < 64, (free_1, free_2)
