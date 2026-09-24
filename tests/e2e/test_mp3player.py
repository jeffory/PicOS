"""MP3 decode loop termination (review: Audio/storage High "The MP3 decode
loop busy-spins on Core 1 when the SD try-lock fails and a partial frame
remains"; Task 14).

The simulator's MP3 player mirrors the firmware decode loop
(src/drivers/mp3_player.c): a file that ends mid-frame leaves a partial
frame buffered with no new data to come, the same state as an SD try-lock
that keeps failing. The old loop retried the refill forever, so the file
never finished and Core 1 hung. Each case runs alone in its own simulator.
"""

import pytest

from helpers import case_params, lua_case_names, write_mp3, write_wav

APP = "mp3_test"
CASES = lua_case_names(APP)

# Known bugs still open, {case: reason} (strict xfails).
KNOWN_BUGS = {}


def _setup(case):
    def setup(sd):
        app = sd / "apps" / APP
        write_mp3(app / "whole.mp3", frames=8)
        whole = (app / "whole.mp3").read_bytes()
        (app / "truncated.mp3").write_bytes(whole[:-200])
        write_wav(app / "tone.wav", seconds=0.5)
        (app / "only.flag").write_text(case)
    return setup


@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_mp3player(lua_suite, case):
    run = lua_suite(APP, setup=_setup(case), timeout=60)
    run.check_case(case)
    run.assert_clean_exit()
