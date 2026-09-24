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

from helpers import E2E_DIR, case_params, lua_case_names, run_lua_app, \
    stage_lua_app, known_bug
from picos_simulator import binary_sanitizers
from net_servers import (BlackholeServer, HttpTestServer, TcpEchoServer,
                         big_body)

pytestmark = pytest.mark.firmware_net

APP = "net_fw"
APP_ID = "com.test.net_fw"
APP_SRC = E2E_DIR / "net_fw" / "main.lua"

# The Lua-side cases, one pytest id each. Two are halves of a Python-driven
# scenario (test_http_inflight_across_app_exit) and one is the app's own
# "unknown case" guard; tcp_leave_open is driven by
# test_tcp_app_exit_closes_sockets.
_DRIVEN = {"http_leave_inflight", "http_after_inflight_exit",
           "tcp_leave_open", "unknown_case"}
CASES = [n for n in lua_case_names(APP_SRC) if n not in _DRIVEN]

TCP_FREE_RACE = ("review: tcp_free race — tcp_free queues CLOSE, then frees "
                 "rx_buf and zeroes the slot before Core 1 runs it, so the "
                 "Mongoose connection stays open with fn_data on the dead "
                 "slot (tcp.c:50-58)")
HTTP_TIMEOUT_UAF = ("review: HTTP timeout-path use-after-free — "
                    "http_check_timeouts drops pcb but keeps fn_data; the "
                    "bridge frees the slot and MG_EV_CLOSE then runs on it "
                    "(http.c:765-789, lua_bridge_network.c:135)")

KNOWN_BUGS = {
    "http_pool_exhaustion":
        "review: each HTTP/TCP object claims its own hardware spinlock "
        "(spin_lock_claim_unused(true), http.c:517) — only ids 24-31 are "
        "claimable, so the 7th live object panics instead of the 9th "
        "failing cleanly",
    "http_connect_timeout": HTTP_TIMEOUT_UAF,
    "http_callbacks_not_reentrant":
        "new: HTTP callbacks re-enter — sys.sleep calls "
        "http_lua_fire_pending with the instruction hook live, and the hook "
        "calls it again from inside the running callback "
        "(lua_bridge_sys.c:62, lua_bridge.c:142)",
    "http_read_timeout":
        "new: the read timeout never arms before the response headers — "
        "the state stays SENDING after MG_EV_CONNECT and "
        "http_check_timeouts only reads deadline_read in HEADERS/BODY, so "
        "a server that accepts and never answers hangs the request forever",
    "http_keepalive_reuse":
        "review: keep-alive reuse always fails — the second request goes "
        "out on the handler the first one detached (wifi.c:237-243)",
    "http_chunked_body":
        "review: the headers handler's mg_iobuf_del detaches Mongoose's "
        "HTTP handler, so chunk-size lines leak into the body and COMPLETE "
        "never fires (http.c:284)",
    "http_close_delimited_body":
        "review: COMPLETE never fires for a close-delimited response "
        "(http.c:284 detach; MG_EV_CLOSE reports CLOSED)",
    "http_post_binary":
        "review: the POST body is copied with http_strdup (stops at the "
        "first NUL) and sent with memcpy(tx_len): truncated and a heap "
        "over-read (http.c:141, 647)",
    "tcp_close_while_receiving": TCP_FREE_RACE,
    "tcp_close_then_reuse_slot": TCP_FREE_RACE,
    "tcp_connect_timeout":
        "review: tcp:setConnectTimeout/setReadTimeout never reach the "
        "connection (lua_bridge_tcp.c:193-203); the 15 s default applies",
}


# Tests take `servers` BEFORE `simulator`: fixtures tear down in reverse, so
# the simulator stops before the servers close their sockets. The other way
# round, a server closing a connection the firmware leaked (tcp_free race)
# delivers MG_EV_CLOSE to a dead slot and crashes the simulator in teardown.
# Found by this suite (not in the review): Lua's sock:connect() passes the
# socket's own host buffer to tcp_connect, which strncpy()s it onto itself.
# Harmless in practice, but undefined behaviour, and ASan aborts on it, so
# under ASan every case that connects a TCP socket stops there.
TCP_STRNCPY_OVERLAP = ("new: l_tcp_connect passes ud->conn->host as the host "
                       "to tcp_connect, which strncpy()s it onto itself "
                       "(lua_bridge_tcp.c:107 -> tcp.c:70); ASan aborts on "
                       "the overlapping strncpy at every TCP connect")
TCP_CONNECTING = {"tcp_echo", "tcp_read_after_close", "tcp_leave_open",
                  "tcp_close_while_receiving", "tcp_close_then_reuse_slot",
                  "tcp_connect_timeout"}


def xfail_tcp_connect_under_asan(request):
    """Strict xfail for a TCP-connecting test on an ASan build (see
    TCP_STRNCPY_OVERLAP); the release and TSan builds run it normally."""
    san = binary_sanitizers(request.config.getoption("--simulator-path")) or ""
    if "address" in san.split(","):
        request.node.add_marker(pytest.mark.xfail(strict=True,
                                                  reason=TCP_STRNCPY_OVERLAP))


@pytest.fixture
def servers():
    http = HttpTestServer().start()
    echo = TcpEchoServer().start()
    hole = BlackholeServer().start()
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
def test_case(request, servers, simulator, case):
    if case in TCP_CONNECTING:
        xfail_tcp_connect_under_asan(request)
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


@known_bug(TCP_FREE_RACE)
def test_tcp_close_releases_server_side(request, servers, simulator):
    """sock:close() must close the connection: the echo server sees EOF."""
    xfail_tcp_connect_under_asan(request)
    run = run_case(simulator, servers, "tcp_echo")
    run.check_case("tcp_echo")
    still_open = servers["echo"].wait_all_closed(timeout=2.0)
    assert still_open == 0, (
        f"{still_open} TCP connection(s) still open 2 s after sock:close() "
        f"(accepted {servers['echo'].accepted})")


@known_bug(TCP_FREE_RACE)
def test_tcp_app_exit_closes_sockets(request, servers, simulator):
    """An app that exits with sockets open must not leave them open:
    teardown GCs the sockets (tcp_free)."""
    xfail_tcp_connect_under_asan(request)
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
