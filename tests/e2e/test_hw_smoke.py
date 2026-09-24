"""The target API itself, on both targets (@pytest.mark.both).

On the simulator (every run) this proves SimTarget; with
--target hw:<port> the same test is the first device check of a
pre-release run: staging, launch, the results file, file transfer both ways,
exit at the launcher, and a screenshot.
"""

from __future__ import annotations

import time

import pytest

from hw_target import pixel

pytestmark = [pytest.mark.both, pytest.mark.timeout(300)]

SMOKE_APP = """
local T = picocalc.sys.loadlib("picotest")
T.case("reads_host_file", function()
    T.eq(picocalc.fs.readFile("/data/" .. APP_ID .. "/in.txt"), "from host 42")
end)
T.case("writes_file", function()
    local f = picocalc.fs.open(picocalc.fs.appPath("out.txt"), "w")
    picocalc.fs.write(f, "from app " .. (6 * 7))
    picocalc.fs.close(f)
end)
T.case("fails_on_purpose", function() T.eq(1 + 1, 3) end)
picocalc.display.clear(picocalc.display.RED)
picocalc.display.flush()
T.done()
"""


def test_target_roundtrip(target):
    app_id = "com.test.hw_smoke"
    target.stage_lua_app("hw_smoke", SMOKE_APP, id=app_id)
    target.write_file(f"/data/{app_id}/in.txt", b"from host 42")

    run = target.run_lua_app("hw_smoke", timeout=30)

    run.assert_clean_exit()
    assert run.cases["reads_host_file"]["status"] == "PASS", run.describe()
    assert run.cases["writes_file"]["status"] == "PASS", run.describe()
    # A failing case is reported as FAIL with its detail, not lost.
    bad = run.cases["fails_on_purpose"]
    assert bad["status"] == "FAIL" and "expected 3, got 2" in bad["detail"], bad
    assert target.read_file(f"/data/{app_id}/out.txt") == b"from app 42"
    with pytest.raises(FileNotFoundError):
        target.read_file(f"/data/{app_id}/never_written.txt")

    # The launcher repaints after the app returns: the red app screen goes.
    deadline = time.monotonic() + 10
    png = target.screenshot()
    while pixel(png, 160, 160) == (255, 0, 0) and time.monotonic() < deadline:
        time.sleep(0.5)
        png = target.screenshot()
    assert png[:4] == b"\x89PNG"
    assert pixel(png, 160, 160) != (255, 0, 0), "screen still shows the app"

    # exit with no app running is refused, and the target still answers.
    r = target.exit_app()
    assert r == {"ok": False, "message": "Error: exit: no app running"}, r
    assert target.status()["app"] == "launcher"
