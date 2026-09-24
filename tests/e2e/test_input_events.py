"""picocalc.input.pollEvent / isKeyDown and the keyboard event queue.

Review row (Audio/storage Medium): the keyboard kept only net state and the
last char per poll, so a second char in the same poll was lost and a tap
shorter than a poll produced nothing; SDK gap 7 asked for an ordered event
API. The fixture (apps/input_events) sleeps without polling while the test
injects, so every injection of a phase lands in ONE input.update().

The STM32 FIFO decoding itself (HOLD = repeat, taps inside one poll, shifted
letter release) is covered by tests/unit/test_kbd_event_queue.c.
"""

BTN_ENTER = 1 << 4
BTN_CTRL = 1 << 20
KEY_ENTER = 0x0A
KEY_MOD_CTRL = 0xA5


def _texts(sim, since):
    return [l.get("text", "") for l in sim.get_log_lines(since)]


def _events(sim, since, upto_marker):
    out = []
    for t in _texts(sim, since):
        if t == upto_marker:
            break
        if t.startswith("IE:EV "):
            out.append(t[len("IE:EV "):])
    return out


def _start(sim):
    seq = sim.get_log_buffer(tail=1).get("next_seq", 0)
    sim.launch_app("input_events")
    sim.wait_for_log(r"^IE:READY 1$", timeout=10, since_seq=seq)
    return seq


def test_poll_event_reports_keys_in_order(simulator):
    sim = simulator
    seq = _start(sim)
    assert "IE:HAS function function" in _texts(sim, seq)
    assert "IE:EMPTY nil" in _texts(sim, seq)

    sim.keypress("a")
    sim.keypress("enter")
    sim.keypress("B")
    sim.wait_for_log(r"^IE:DONE 1$", timeout=10, since_seq=seq)

    # type key char mods button repeat
    assert _events(sim, seq, "IE:DONE 1") == [
        "down 97 - 0 - -",
        "char 97 97 0 - -",
        "up 97 - 0 - -",
        f"down {KEY_ENTER} - 0 {BTN_ENTER} -",
        "down 66 - 0 - -",
        "char 66 66 0 - -",
        "up 66 - 0 - -",
        f"up {KEY_ENTER} - 0 {BTN_ENTER} -",
    ]
    assert "IE:NIL nil" in _texts(sim, seq)


def test_two_chars_in_one_frame_both_arrive(simulator):
    sim = simulator
    seq = _start(sim)
    sim.wait_for_log(r"^IE:DONE 1$", timeout=10, since_seq=seq)
    sim.wait_for_log(r"^IE:READY 2$", timeout=10, since_seq=seq)

    sim.keypress("x")
    sim.keypress("y")
    sim.wait_for_log(r"^IE:DONE 2$", timeout=10, since_seq=seq)

    texts = _texts(sim, seq)
    assert "IE:GETCHAR xy" in texts, texts
    start = texts.index("IE:DONE 1")
    phase2 = [t[len("IE:EV "):] for t in texts[start:texts.index("IE:DONE 2")]
              if t.startswith("IE:EV ")]
    assert [e for e in phase2 if e.startswith("char")] == [
        "char 120 120 0 - -", "char 121 121 0 - -"]


def test_is_key_down_follows_a_held_key(simulator):
    sim = simulator
    seq = _start(sim)
    sim.wait_for_log(r"^IE:READY 3$", timeout=15, since_seq=seq)

    sim.call("inject_button", {"button": "ctrl", "action": "press"})
    sim.keypress("k")
    sim.wait_for_log(r"^IE:HELD 3$", timeout=10, since_seq=seq)
    texts = _texts(sim, seq)
    assert "IE:DOWN ctrl=true k=false K=false held=true" in texts, texts
    start = texts.index("IE:DONE 2")
    phase3 = [t[len("IE:EV "):] for t in texts[start:] if t.startswith("IE:EV ")]
    assert phase3 == [
        f"down {KEY_MOD_CTRL} - {BTN_CTRL} {BTN_CTRL} -",
        f"down 107 - {BTN_CTRL} - -",
        f"char 107 107 {BTN_CTRL} - -",
        f"up 107 - {BTN_CTRL} - -",
    ]

    sim.call("inject_button", {"button": "ctrl", "action": "release"})
    sim.wait_for_log(r"^IE:DONE 3$", timeout=10, since_seq=seq)
    texts = _texts(sim, seq)
    assert "IE:RELEASED true" in texts, texts
    assert f"IE:EV up {KEY_MOD_CTRL} - 0 {BTN_CTRL} -" in texts, texts
    assert "IE:BADARGS false false" in texts, texts


def test_clear_state_empties_the_queue(simulator):
    sim = simulator
    seq = _start(sim)
    sim.wait_for_log(r"^IE:READY 3$", timeout=15, since_seq=seq)
    sim.call("inject_button", {"button": "ctrl", "action": "press"})
    sim.wait_for_log(r"^IE:HELD 3$", timeout=10, since_seq=seq)
    sim.call("inject_button", {"button": "ctrl", "action": "release"})
    sim.wait_for_log(r"^IE:READY 4$", timeout=10, since_seq=seq)
    sim.keypress("z")
    sim.wait_for_log(r"^IE:DONE 4$", timeout=10, since_seq=seq)
    assert "IE:CLEARED nil" in _texts(sim, seq)


def test_new_app_does_not_inherit_queued_keys(simulator):
    """Keys typed at the launcher (which polls but never pops events) must
    not show up in the next app's pollEvent."""
    sim = simulator
    for ch in "launcher":
        sim.keypress(ch)
    sim.keypress("down")
    sim.keypress("up")
    seq = _start(sim)
    texts = _texts(sim, seq)
    assert "IE:HAS function function" in texts
    assert "IE:EMPTY nil" in texts, texts
