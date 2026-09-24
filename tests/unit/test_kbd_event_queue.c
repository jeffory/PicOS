// Host unit tests for src/drivers/kbd_event_queue.h: the STM32 FIFO decoder
// and the keyboard event queue behind picocalc.input.pollEvent / isKeyDown.
// Each test drives kbd_fifo_apply() with the (state, keycode) items the
// keyboard controller's FIFO returns, the way kbd_poll() does on hardware.
#include "check.h"
#include "kbd_event_queue.h"

static kbd_input_t in;
static kbd_buttons_t btn;

static void reset(void) {
  kbd_input_clear(&in);
  memset(&btn, 0, sizeof(btn));
}

static void begin_poll(void) {
  kbd_buttons_begin_poll(&btn);
  in.ev_pushed = 0;
  in.char_pushed = 0;
}

static uint32_t pressed(void) { return btn.curr & ~btn.prev; }
static uint32_t released(void) { return ~btn.curr & btn.prev; }

static kbd_event_t pop(void) {
  kbd_event_t e = {0, 0, 0, 0};
  kbd_evq_pop(&in.q, &e);
  return e;
}

#define CHECK_EV(e, t, k, c, f) do { kbd_event_t _e = (e); \
  CHECK_EQ_INT(_e.type, t); CHECK_EQ_INT(_e.key, k); \
  CHECK_EQ_INT(_e.ch, c); CHECK_EQ_INT(_e.flags, f); } while (0)

// Two chars in one poll: both are queued in order, and getChar's backlog
// hands them out one per poll instead of keeping only the last.
static void test_two_chars_in_one_poll(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'a');
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'b');
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, 'a');
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, 'b');
  CHECK_EQ_INT(in.q.count, 6);
  CHECK_EV(pop(), KBD_EV_DOWN, 'a', 0, 0);
  CHECK_EV(pop(), KBD_EV_CHAR, 'a', 'a', 0);
  CHECK_EV(pop(), KBD_EV_DOWN, 'b', 0, 0);
  CHECK_EV(pop(), KBD_EV_CHAR, 'b', 'b', 0);
  CHECK_EV(pop(), KBD_EV_UP, 'a', 0, 0);
  CHECK_EV(pop(), KBD_EV_UP, 'b', 0, 0);
  CHECK(!kbd_evq_pop(&in.q, NULL));
  CHECK_EQ_INT(kbd_chars_pop(&in), 'a');
  CHECK_EQ_INT(kbd_chars_pop(&in), 'b');
  CHECK_EQ_INT(kbd_chars_pop(&in), 0);
}

// Letters report key-up: isKeyDown follows press and release.
static void test_letter_key_down_and_up(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'q');
  CHECK(kbd_keyset_test(&in.down, 'q'));
  CHECK(!kbd_keyset_test(&in.down, 'w'));
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, 'q');
  CHECK(!kbd_keyset_test(&in.down, 'q'));
}

// Shift can come up before the letter, so the release names 'a' after a
// press of 'A': the key-down set is case-folded and the key does not stick.
static void test_shifted_letter_release(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_MOD_SHL);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'A');
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_MOD_SHL);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, 'a');
  CHECK(!kbd_keyset_test(&in.down, 'a'));
  CHECK(!kbd_keyset_test(&in.down, 'A'));
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_MOD_SHL, 0, KBD_MOD_SHIFT);
  CHECK_EV(pop(), KBD_EV_DOWN, 'A', 0, KBD_MOD_SHIFT);
  CHECK_EV(pop(), KBD_EV_CHAR, 'A', 'A', KBD_MOD_SHIFT);
  CHECK_EV(pop(), KBD_EV_UP, KEY_MOD_SHL, 0, 0);
  CHECK_EV(pop(), KBD_EV_UP, 'a', 0, 0);
}

// A HOLD for a key whose press was seen is a repeat: flagged events and a
// repeated char, and no fresh press edge.
static void test_hold_is_a_repeat(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_ENTER);
  CHECK_EQ_U32(pressed(), BTN_ENTER);
  kbd_evq_clear(&in.q);
  kbd_chars_pop(&in);
  begin_poll();
  CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, KEY_ENTER), KEY_ENTER);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(btn.curr, BTN_ENTER);
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_ENTER, 0, KBD_EVF_REPEAT);
  CHECK_EV(pop(), KBD_EV_CHAR, KEY_ENTER, '\n', KBD_EVF_REPEAT);
  CHECK_EQ_INT(kbd_chars_pop(&in), '\n');
}

// A HOLD for a key whose press was not seen (it was pressed before
// kbd_clear_state / kbd_discard_pending, e.g. before ui_confirm opened) is
// not a press: no press edge, no event, no char. Its release is still an up
// event, and a later real press is a normal press.
static void test_hold_after_clear_is_not_a_press(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_ENTER);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'y');
  reset(); // kbd_discard_pending
  begin_poll();
  CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, KEY_ENTER), 0);
  CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, 'y'), 0);
  CHECK_EQ_U32(pressed(), 0);
  CHECK_EQ_U32(btn.curr, BTN_ENTER); // held, truthfully
  CHECK_EQ_INT(in.q.count, 0);
  CHECK_EQ_INT(kbd_chars_pop(&in), 0);
  CHECK(kbd_keyset_test(&in.down, 'y'));
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_ENTER);
  CHECK_EQ_U32(released(), BTN_ENTER);
  CHECK_EV(pop(), KBD_EV_UP, KEY_ENTER, 0, 0);
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_ENTER);
  CHECK_EQ_U32(pressed(), BTN_ENTER);
}

// A button pressed and released inside one 50 ms poll used to produce no
// edge at all. It now reads as held (with a press edge) for that poll and is
// released at the next.
// The STM32 may repeat HOLD. A key held across a clear stays quiet on every
// HOLD (not just the first) until it is released and pressed again: a 'y'
// held from typing when ui_confirm opens can never answer it.
static void test_repeated_hold_after_clear_stays_quiet(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'y');
  reset(); // kbd_discard_pending
  for (int i = 0; i < 3; i++) {
    begin_poll();
    CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, 'y'), 0);
    CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, KEY_ENTER), 0);
    CHECK_EQ_U32(pressed(), 0);
  }
  CHECK_EQ_INT(in.q.count, 0);
  CHECK_EQ_INT(kbd_chars_pop(&in), 0);
  CHECK(kbd_keyset_test(&in.down, 'y')); // physically held, truthfully
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, 'y');
  kbd_evq_clear(&in.q);
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 'y');
  kbd_chars_pop(&in);
  kbd_evq_clear(&in.q);
  begin_poll();
  CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_HOLD, 'y'), 'y');
  CHECK_EV(pop(), KBD_EV_DOWN, 'y', 0, KBD_EVF_REPEAT);
  CHECK_EQ_INT(kbd_chars_pop(&in), 'y'); // a seen press repeats again
}

static void test_tap_inside_one_poll(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_UP);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_UP);
  CHECK_EQ_U32(pressed(), BTN_UP);
  CHECK_EQ_U32(btn.curr, BTN_UP);
  CHECK(!kbd_keyset_test(&in.down, KEY_UP));
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_UP, 0, 0);
  CHECK_EV(pop(), KBD_EV_UP, KEY_UP, 0, 0);
  begin_poll();
  CHECK_EQ_U32(btn.curr, 0);
  CHECK_EQ_U32(released(), BTN_UP);
  begin_poll();
  CHECK_EQ_U32(released(), 0);
}

// Press, release, press again inside one poll: the key ends up held.
static void test_tap_then_press_again(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_UP);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_UP);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_UP);
  begin_poll();
  CHECK_EQ_U32(btn.curr, BTN_UP);
}

static void test_ctrl_letter(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_MOD_CTRL);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, 's');
  kbd_evq_pop(&in.q, NULL); // ctrl down
  CHECK_EV(pop(), KBD_EV_DOWN, 's', 0, KBD_MOD_CTRL);
  CHECK_EV(pop(), KBD_EV_CHAR, 's', 0x13, KBD_MOD_CTRL);
  CHECK_EQ_INT(kbd_chars_pop(&in), 0x13);
}

// Menu (F10) and Brk are the OS's: they drive the button mask / raw key but
// never enter the queue.
static void test_os_keys_not_queued(void) {
  reset();
  begin_poll();
  kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_F10);
  CHECK_EQ_U32(btn.curr, BTN_MENU);
  CHECK_EQ_INT(kbd_fifo_apply(&in, &btn, KBD_FIFO_PRESSED, KEY_BRK), KEY_BRK);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_F10);
  kbd_fifo_apply(&in, &btn, KBD_FIFO_RELEASED, KEY_BRK);
  CHECK_EQ_INT(in.q.count, 0);
}

static void test_queue_drops_oldest(void) {
  reset();
  for (int i = 0; i < KBD_EVENT_QUEUE_LEN + 3; i++)
    kbd_input_push(&in, KBD_EV_DOWN, (uint8_t)(0x20 + i), 0, 0);
  CHECK_EQ_INT(in.q.count, KBD_EVENT_QUEUE_LEN);
  CHECK_EQ_INT(pop().key, 0x23);
  for (int i = 1; i < KBD_EVENT_QUEUE_LEN; i++)
    CHECK_EQ_INT(pop().key, 0x23 + i);
  CHECK(!kbd_evq_pop(&in.q, NULL));
}

static void test_char_backlog_drops_oldest(void) {
  reset();
  for (int i = 0; i < KBD_CHAR_BACKLOG + 2; i++)
    kbd_chars_push(&in, (char)('a' + i));
  CHECK_EQ_INT(kbd_chars_pop(&in), 'c');
  kbd_chars_drop_newest(&in, 1);
  for (int i = 1; i < KBD_CHAR_BACKLOG - 1; i++)
    CHECK_EQ_INT(kbd_chars_pop(&in), 'c' + i);
  CHECK_EQ_INT(kbd_chars_pop(&in), 0);
}

// The idle-dim wake swallow drops the waking poll's presses and chars but
// keeps its releases, so a key the app saw go down still comes up.
// ...and drops the ups of keys pressed in the waking poll (their downs are
// dropped too, so the ups would be orphans).
static void test_drop_newest_keeps_ups(void) {
  reset();
  kbd_keyset_t before;
  memset(&before, 0, sizeof(before));
  kbd_keyset_set(&before, 'x');
  kbd_input_push(&in, KBD_EV_DOWN, 'x', 0, 0);  // an earlier poll
  kbd_input_push(&in, KBD_EV_UP, 'x', 0, 0);    // this poll ...
  kbd_input_push(&in, KBD_EV_DOWN, 'y', 0, 0);
  kbd_input_push(&in, KBD_EV_CHAR, 'y', 'y', 0);
  kbd_input_push(&in, KBD_EV_UP, 'y', 0, 0);
  kbd_evq_drop_newest_keep_ups(&in.q, 4, &before);
  CHECK_EQ_INT(in.q.count, 2);
  CHECK_EV(pop(), KBD_EV_DOWN, 'x', 0, 0);
  CHECK_EV(pop(), KBD_EV_UP, 'x', 0, 0);
}

static void test_injected_button_events(void) {
  reset();
  kbd_input_button_events(&in, BTN_ESC | BTN_CTRL | BTN_MENU, KBD_EV_DOWN,
                          BTN_ESC | BTN_CTRL);
  CHECK(kbd_keyset_test(&in.down, KEY_ESC));
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_ESC, 0, KBD_MOD_CTRL);
  CHECK_EV(pop(), KBD_EV_DOWN, KEY_MOD_CTRL, 0, KBD_MOD_CTRL);
  CHECK(!kbd_evq_pop(&in.q, NULL)); // MENU is the OS's
  kbd_input_button_events(&in, BTN_ESC | BTN_UP, KBD_EV_UP, 0);
  CHECK_EV(pop(), KBD_EV_UP, KEY_ESC, 0, 0);
  CHECK(!kbd_evq_pop(&in.q, NULL)); // UP was never down
}

static void test_keycode_button_map(void) {
  CHECK_EQ_U32(kbd_keycode_to_button(KEY_MOD_SHR), BTN_SHIFT);
  CHECK_EQ_INT(kbd_button_to_keycode(BTN_SHIFT), KEY_MOD_SHL);
  CHECK_EQ_INT(kbd_button_to_keycode(BTN_F9), KEY_F9);
  CHECK_EQ_U32(kbd_keycode_to_button('a'), 0);
  CHECK_EQ_U32(kbd_buttons_from_mods(kbd_mods_from_buttons(0xFFFFFFFFu)),
               KBD_MOD_BUTTONS);
}

int main(void) {
  test_two_chars_in_one_poll();
  test_letter_key_down_and_up();
  test_shifted_letter_release();
  test_hold_is_a_repeat();
  test_hold_after_clear_is_not_a_press();
  test_repeated_hold_after_clear_stays_quiet();
  test_tap_inside_one_poll();
  test_tap_then_press_again();
  test_ctrl_letter();
  test_os_keys_not_queued();
  test_queue_drops_oldest();
  test_char_backlog_drops_oldest();
  test_drop_newest_keeps_ups();
  test_injected_button_events();
  test_keycode_button_map();
  return check_report("test_kbd_event_queue");
}
