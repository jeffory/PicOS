"""Simulator process basics: it starts, auto-launches, and exits on SIGTERM.

These spawn the binary directly (not through PicodeckSimulator) because the
behaviour under test is the raw process lifecycle. Paths are absolute and the
SD card is a per-test tmp dir, so nothing is written into the repo.
"""

import json
import os
import re
import signal
import socket
import subprocess
import threading
import time

import pytest

from helpers import build_sd_card
from picodeck_simulator import sanitizer_env


def _spawn(binary, sd, *extra):
    env = sanitizer_env(os.environ.copy())
    env["SDL_VIDEODRIVER"] = "dummy"
    env["SDL_AUDIODRIVER"] = "dummy"
    cmd = [str(binary), "--sd-card", str(sd), "--port", "0",
           "--unix-socket", "none", *extra]
    return subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, env=env)


class _Output:
    """Drains the child's stdout on a thread (a full pipe would block it)."""

    def __init__(self, proc):
        self.lines = []
        self._cond = threading.Condition()
        self._t = threading.Thread(target=self._drain, args=(proc,), daemon=True)
        self._t.start()

    def _drain(self, proc):
        for raw in iter(proc.stdout.readline, b""):
            with self._cond:
                self.lines.append(raw.decode("utf-8", errors="replace").rstrip())
                self._cond.notify_all()

    def wait_for(self, text, timeout=10.0):
        deadline = time.time() + timeout
        with self._cond:
            while not any(text in l for l in self.lines):
                remaining = deadline - time.time()
                if remaining <= 0:
                    raise TimeoutError(f"{text!r} not in output:\n"
                                       + "\n".join(self.lines[-40:]))
                self._cond.wait(remaining)

    def text(self):
        self._t.join(timeout=2)
        return "\n".join(self.lines)


@pytest.fixture
def sd(tmp_path):
    return build_sd_card(tmp_path / "sd_card")


def _rpc(out, method, timeout=2.0):
    """One JSON-RPC call over the port the simulator printed."""
    port = int(re.search(r"TCP port: (\d+)", "\n".join(out.lines)).group(1))
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as s:
        s.sendall((json.dumps({"jsonrpc": "2.0", "id": 1, "method": method,
                               "params": {}}) + "\n").encode())
        buf = b""
        while b"\n" not in buf:
            chunk = s.recv(65536)
            if not chunk:
                break
            buf += chunk
    return json.loads(buf.split(b"\n", 1)[0]).get("result")


def _wait_running(out, timeout=10.0):
    """Wait until get_running_app names an app (stdout is block-buffered
    into a pipe, so it can't be used to see the app start)."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        status = _rpc(out, "get_running_app")
        if status and status.get("name"):
            return status
        time.sleep(0.05)
    raise TimeoutError("no app running")


def _terminate(proc, timeout=5.0) -> float:
    start = time.time()
    proc.send_signal(signal.SIGTERM)
    try:
        proc.wait(timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        proc.wait()
        pytest.fail(f"EXIT HANG: simulator ignored SIGTERM for {timeout}s")
    return time.time() - start


class TestBasicLaunchExit:

    def test_simulator_launches_and_exits_cleanly(self, simulator_binary, sd):
        proc = _spawn(simulator_binary, sd)
        out = _Output(proc)
        try:
            out.wait_for("[Socket] TCP port:")
            assert proc.poll() is None, "Simulator exited prematurely"
            _terminate(proc)
            assert proc.returncode == 0, (
                f"SIGTERM should shut down cleanly, got {proc.returncode}\n"
                + out.text()[-2000:])
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

    def test_simulator_auto_launch_and_exit(self, simulator_binary, sd):
        proc = _spawn(simulator_binary, sd, "--launch", "hello")
        out = _Output(proc)
        try:
            out.wait_for("[Socket] TCP port:")
            status = _wait_running(out)
            assert status["name"] == "Hello World", status
            assert proc.poll() is None, "Simulator exited prematurely"
            _terminate(proc, timeout=8)
            assert proc.returncode == 0, out.text()[-2000:]
            assert "App 'Hello World' exited" in out.text(), out.text()[-2000:]
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()

    def test_simulator_exit_without_hang(self, simulator_binary, sd):
        """Regression: SIGTERM shortly after boot must not hang the exit."""
        proc = _spawn(simulator_binary, sd)
        out = _Output(proc)
        try:
            out.wait_for("[Socket] TCP port:")
            took = _terminate(proc)
            assert took < 5, f"Exit took too long: {took:.1f}s"
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
