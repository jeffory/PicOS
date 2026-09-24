"""Simulator test-control channel (audit R1/R11/R12).

Covers the RPCs and notifications the harness synchronises on:
- get_log_buffer {since_seq}: sequenced, sourced entries; paging; ring drop
- `log` notifications (subscribe {"logs": true})
- app.exited {name, id, found, result, error, runtime_ms}
- launch_app {"queued", "busy"}, rescan-on-miss, rescan_apps
- --test-mode: error text reaches the log at once, error screens don't block
- display_stats.present_count (wait_frames) and input seqs (get_input_state)
- boot parity (config_load at boot), SD-root containment, no dofile/loadfile
"""

import json
import socket
import time

import pytest

from picos_simulator import PicosSimulator


@pytest.fixture
def harness_sim(request, simulator_binary, test_sd_card):
    """Fresh --test-mode simulator on the per-test SD card.

    Writes /system/config.json before boot so boot-time config_load() is
    observable (harness_ok logs H:CFG <harness_key>).
    """
    (test_sd_card / "system" / "config.json").write_text(
        json.dumps({"harness_key": "42"}))
    sim = PicosSimulator(
        binary_path=str(simulator_binary),
        sd_card_path=str(test_sd_card),
        headless=True,
        test_mode=True,
    )
    sim.start()
    yield sim
    sim.stop()


def _run(sim, name, timeout=15.0):
    sim.launch_app(name)
    return sim.wait_for_exit(timeout=timeout)


def _texts(entries, src=None):
    return [e["text"] for e in entries if src is None or e["src"] == src]


# ── Log channel ─────────────────────────────────────────────────────────────


def test_log_entries_are_sequenced_and_since_seq_is_honoured(harness_sim):
    sim = harness_sim
    _run(sim, "harness_ok")
    page = sim.get_log_buffer(0)
    lines = page["lines"]
    assert lines and all(isinstance(e, dict) for e in lines), page
    seqs = [e["seq"] for e in lines]
    assert seqs == sorted(seqs) and len(set(seqs)) == len(seqs), seqs
    assert all({"seq", "t_ms", "src", "text"} <= set(e) for e in lines)
    assert page["next_seq"] == seqs[-1] + 1
    assert page["dropped"] == 0

    app_lines = [e for e in lines if e["text"].startswith("H:")]
    assert all(e["src"] == "lua" for e in app_lines), app_lines
    line3 = next(e for e in lines if e["text"] == "H:LINE 3")

    newer = sim.get_log_buffer(since_seq=line3["seq"])["lines"]
    assert newer[0]["seq"] == line3["seq"]
    assert "H:LINE 2" not in _texts(newer) and "H:LINE 4" in _texts(newer)

    assert sim.get_log_buffer(since_seq=page["next_seq"])["lines"] == []

    tail = sim.get_log_buffer(tail=2)["lines"]
    assert [e["seq"] for e in tail] == seqs[-2:]

    # Launcher events arrive on the "os" source.
    assert any("start Harness OK" in t for t in _texts(lines, "os")), lines


def test_log_ring_keeps_newest_lines_and_reports_drops(harness_sim):
    sim = harness_sim
    _run(sim, "harness_flood", timeout=30.0)
    first = sim.get_log_buffer(0)
    assert first["dropped"] > 0, "5000 lines overflowed a 4096-line ring"
    entries = sim.get_log_lines(0)
    texts = _texts(entries)
    assert "H:FLOOD_DONE" in texts, texts[-5:]      # newest kept, not oldest
    assert "H:FLOOD 1 " + "x" * 200 not in texts
    assert len(entries) == 4096
    # Long lines survive whole (the old ring cut them at 256 chars).
    assert any(t.startswith("H:FLOOD 4999 ") and t.endswith("x" * 200) for t in texts)


def test_log_notifications_are_pushed_to_subscribers(harness_sim):
    sim = harness_sim
    assert sim.subscribe_logs(True) == {"logs": True}
    sim.launch_app("harness_ok")
    sim.wait_for_exit(timeout=15.0)
    deadline = time.time() + 2.0
    while time.time() < deadline:
        pushed = [e for e in list(sim._log_events) if e.get("text") == "H:DONE"]
        if pushed:
            break
        time.sleep(0.02)
    assert pushed and pushed[0]["src"] == "lua" and pushed[0]["seq"] > 0


# ── App outcome ─────────────────────────────────────────────────────────────


def test_app_exited_reports_returned(harness_sim):
    out = _run(harness_sim, "harness_ok")
    assert out["found"] is True
    assert out["result"] == "returned", out
    assert out["id"] == "com.picos.harness_ok"
    assert out["name"] == "harness_ok"
    assert out["error"] is None
    assert isinstance(out["runtime_ms"], int) and out["runtime_ms"] >= 0


def test_app_exited_reports_runtime_error_and_logs_it_at_once(harness_sim):
    sim = harness_sim
    sim.subscribe_logs(True)
    t0 = time.time()
    sim.launch_app("harness_err")
    text = sim.wait_for_log(r"harness boom 42", timeout=1.0, src="err")
    assert time.time() - t0 < 1.0
    assert "Runtime error:" in text
    sim.wait_for_log(r"stack traceback", timeout=1.0, src="err")

    out = sim.wait_for_exit(timeout=5.0)
    assert out["result"] == "error", out
    assert "harness boom 42" in out["error"]
    assert out["runtime_ms"] < 3000, "--test-mode must not wait on the error screen"


def test_app_exited_reports_exit_sentinel(harness_sim):
    out = _run(harness_sim, "harness_exit")
    assert out["result"] == "exit_sentinel", out
    texts = _texts(harness_sim.get_log_lines(0))
    assert "H:EXIT_START" in texts and "H:NOT_REACHED" not in texts


def test_unknown_app_reports_not_found(harness_sim):
    out = _run(harness_sim, "no_such_app_xyz")
    assert out["found"] is False
    assert out["result"] == "load_failed"
    assert "not found" in out["error"]


def test_min_psram_refusal_is_load_failed_and_logged(harness_sim):
    sim = harness_sim
    out = _run(sim, "harness_bigmem")
    assert out["result"] == "load_failed", out
    assert "contiguous" in out["error"]
    errs = _texts(sim.get_log_lines(0), "err")
    assert any("not enough PSRAM" in t for t in errs), errs
    assert "H:BIGMEM_RAN" not in _texts(sim.get_log_lines(0))


# ── Launching ───────────────────────────────────────────────────────────────


def test_launch_app_staged_after_boot(harness_sim, test_sd_card):
    app = test_sd_card / "apps" / "harness_late"
    app.mkdir()
    (app / "app.json").write_text(json.dumps(
        {"id": "com.picos.harness_late", "name": "Harness Late"}))
    (app / "main.lua").write_text('picocalc.sys.log("H:LATE")\n')

    out = _run(harness_sim, "harness_late")
    assert out["found"] is True and out["result"] == "returned", out
    assert "H:LATE" in _texts(harness_sim.get_log_lines(0))


def test_rescan_apps_rescans_the_launcher(harness_sim):
    sim = harness_sim
    assert sim.rescan_apps() == {"queued": True}
    sim.wait_for_log(r"apps rescanned", timeout=2.0, src="os")


def test_launch_app_reports_queued_and_busy(harness_sim):
    sim = harness_sim
    assert sim.launch_app("harness_input") == {"queued": True, "busy": False}
    sim.wait_for_log(r"^H:INPUT_READY$", timeout=5.0)
    # A second launch while the first app runs is queued behind it.
    assert sim.call("launch_app", {"name": "harness_ok"}) == {"queued": True, "busy": True}
    sim.keypress("q")
    first = sim.wait_for_notification("app.exited", timeout=5.0)["params"]
    assert first["name"] == "harness_input" and first["result"] == "returned"
    second = sim.wait_for_notification("app.exited", timeout=5.0)["params"]
    assert second["name"] == "harness_ok" and second["result"] == "returned"


# ── Frame and input sync ────────────────────────────────────────────────────


def test_wait_frames_counts_presents(harness_sim):
    sim = harness_sim
    sim.launch_app("harness_input")
    sim.wait_for_log(r"^H:INPUT_READY$", timeout=5.0)
    before = sim.present_count()
    after = sim.wait_frames(5, timeout=3.0)
    assert after >= before + 5
    sim.keypress("q")
    sim.wait_for_exit(timeout=5.0)


def test_injections_return_seqs_that_get_consumed(harness_sim):
    sim = harness_sim
    sim.launch_app("harness_input")
    sim.wait_for_log(r"^H:INPUT_READY$", timeout=5.0)

    r1 = sim.keypress("a")
    r2 = sim.keypress("enter")
    assert r2["input_seq"] > r1["input_seq"] > 0
    state = sim.wait_input_consumed(r2["input_seq"], timeout=2.0)
    assert state["consumed_seq"] >= r2["input_seq"]
    assert state["issued_seq"] >= r2["input_seq"]
    sim.wait_for_log(r"^H:CHAR a$", timeout=2.0)
    sim.wait_for_log(r"^H:ENTER$", timeout=2.0)

    sim.keypress("q")
    sim.wait_for_exit(timeout=5.0)


# ── Boot parity and containment ─────────────────────────────────────────────


def test_config_is_loaded_at_boot(harness_sim):
    _run(harness_sim, "harness_ok")
    assert "H:CFG 42" in _texts(harness_sim.get_log_lines(0))


def test_dofile_and_loadfile_are_removed(harness_sim):
    _run(harness_sim, "harness_ok")
    assert "H:DOFILE nil nil" in _texts(harness_sim.get_log_lines(0))


def test_sd_escape_is_refused(harness_sim, test_sd_card):
    sim = harness_sim
    outside = test_sd_card.parent / "escape_probe.json"
    out = _run(sim, "harness_fs")
    assert out["result"] == "returned", out
    entries = sim.get_log_lines(0)
    assert not outside.exists(), "game.save wrote outside the SD root"
    assert "H:ESCAPE false failed to open file for writing" in _texts(entries)
    assert any("[SIM] SD escape: /saves/../../escape_probe.json" in t
               for t in _texts(entries, "err")), _texts(entries, "err")
    # A '..' that stays inside the root still resolves.
    assert "H:INSIDE true" in _texts(entries)
    assert (test_sd_card / "saves" / "inside_probe.json").exists()


# ── Slow clients ────────────────────────────────────────────────────────────


def _raw_client(port):
    s = socket.create_connection(("127.0.0.1", port))
    s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    return s


def test_stalled_client_is_disconnected_without_stalling_sys_log(harness_sim):
    sim = harness_sim
    sim.subscribe_logs(True)
    sim.launch_app("harness_ticker")
    sim.wait_for_log(r"^H:TICK 1$", timeout=5.0)

    # A second client asks for ~16 MB of screenshots and never reads.
    stalled = _raw_client(sim.tcp_port)
    reqs = "".join(json.dumps({"jsonrpc": "2.0", "id": i, "method": "screenshot",
                               "params": {"format": "raw"}}) + "\n"
                   for i in range(60))
    stalled.sendall(reqs.encode())

    # While the socket thread is stuck on it, ticks keep reaching client 1.
    arrivals = {}
    t_end = time.time() + 6.5
    while time.time() < t_end:
        with sim._notif_cond:
            for e in sim._log_events:
                if e.get("text", "").startswith("H:TICK ") and e["seq"] not in arrivals:
                    arrivals[e["seq"]] = time.time()
        time.sleep(0.02)
    times = sorted(arrivals.values())
    assert len(times) > 100, f"only {len(times)} ticks arrived during the stall"
    worst = max(b - a for a, b in zip(times, times[1:]))
    assert worst < 1.0, f"log delivery to client 1 stalled for {worst:.2f}s"

    # The stalled client is dropped (after the 5 s drain timeout): draining
    # its socket ends in EOF rather than an endless stream.
    stalled.settimeout(10.0)
    got_eof = False
    deadline = time.time() + 20.0
    while time.time() < deadline:
        try:
            chunk = stalled.recv(1 << 20)
        except ConnectionResetError:  # closed with our requests unread
            chunk = b""
        if not chunk:
            got_eof = True
            break
    stalled.close()
    assert got_eof, "stalled client was never disconnected"

    # sys.log itself never blocked: server-side tick timestamps stay dense.
    ticks = [e for e in sim.get_log_lines(0) if e["text"].startswith("H:TICK ")]
    t_ms = [e["t_ms"] for e in ticks]
    assert max(b - a for a, b in zip(t_ms, t_ms[1:])) < 500, "sys.log stalled"
    sim.wait_for_exit(timeout=15.0)
