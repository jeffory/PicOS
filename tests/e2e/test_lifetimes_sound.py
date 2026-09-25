"""Sound object lifetimes (review: Audio/storage High "A sampleplayer doesn't
keep a reference to its sample…", Docs vs implementation volume range; Task
11).

sound_lifetimes runs each case alone in its own simulator, under the default
collector and under a very aggressive incremental one (stress.flag): before
the fix a collected sample is still mixed by the simulator's Core 1 thread,
and one case's crash must not cost the others their result. The ASan
simulator reports the bad access itself; the release simulator sees the
weak-table survival checks, the identity checks and the slot counts.

test_lifetimes.py keeps the older gc_test cases for the same bug.
"""

import pytest

from helpers import case_params, lua_case_names, write_wav

APP = "sound_lifetimes"
CASES = lua_case_names(APP)

# Known bugs still open, {case: reason} (strict xfails).
KNOWN_BUGS = {}


def _setup(case, mode):
    def setup(sd):
        app = sd / "apps" / APP
        write_wav(app / "short.wav", seconds=0.05)   # 1102 frames
        write_wav(app / "long.wav", seconds=0.2)     # 4410 frames
        (app / "only.flag").write_text(case)
        if mode == "stress":
            (app / "stress.flag").write_text("1")
    return setup


@pytest.mark.parametrize("mode", ["normal", "stress"])
@pytest.mark.parametrize("case", case_params(CASES, KNOWN_BUGS))
def test_sound_lifetime(lua_suite, mode, case):
    run = lua_suite(APP, setup=_setup(case, mode), timeout=60)
    run.check_case(case)
    run.assert_clean_exit()
