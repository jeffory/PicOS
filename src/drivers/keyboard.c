#include "keyboard.h"
#include "../hardware.h"
#include "../os/os.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#include <stdio.h>

// Key state values from the STM32 firmware (fifo_item.state)
#define KEY_STATE_IDLE 0
#define KEY_STATE_PRESSED 1
#define KEY_STATE_HOLD 2
#define KEY_STATE_RELEASED 3

// The STM32 uses a STOP-based protocol (not repeated-start):
//   1. Write register address as a complete transaction (nostop=false)
//   2. Wait for the STM32 to prepare its response
//   3. Read in a separate transaction
// pelrun/uf2loader used sleep_ms(16), but that's too slow for 60fps apps.
// Testing shows 1ms is reliable and gives us ~60 FPS.
#define KBD_REG_DELAY_MS 1
#define KBD_I2C_TIMEOUT_US 50000

// ── Internal state
// ────────────────────────────────────────────────────────────

static uint32_t s_buttons_prev = 0;
static uint32_t s_buttons_curr = 0;
static char s_last_char = 0;
static uint8_t s_last_raw_key =
    0; // raw keycode of last press this frame (0 = none)
static bool s_menu_pressed =
    false; // set on BTN_MENU rising edge; cleared by kbd_consume_menu_press()
static bool s_screenshot_pressed =
    false; // set on KEY_BRK press; cleared by kbd_consume_screenshot_press()
static int s_i2c_fail_count = 0; // consecutive kbd_poll() I2C failures

// ── Public API
// ────────────────────────────────────────────────────────────────

// ── I2C helpers
// ───────────────────────────────────────────────────────────────

// Recover the I2C bus from a stuck state by:
//   1. De-initing the I2C peripheral to release GPIO control
//   2. Pulsing SCL 9 times to clock out any partial byte the STM32 is stuck in
//   3. Issuing an explicit STOP if SDA is still stuck low
//   4. Re-initing the I2C peripheral
// Safe to call during kbd_init() (before first i2c_init) and at runtime.
static void kbd_recover_i2c_bus(void) {
  // Release the I2C peripheral so we can drive the pins manually.
  i2c_deinit(KBD_I2C_PORT);

  // SDA as floating input (pulled high) — no START generated during SCL pulses.
  gpio_init(KBD_PIN_SDA);
  gpio_set_dir(KBD_PIN_SDA, GPIO_IN);
  gpio_pull_up(KBD_PIN_SDA);
  sleep_us(200);

  // Pre-load SCL HIGH before driving it as an output.
  gpio_init(KBD_PIN_SCL);
  gpio_put(KBD_PIN_SCL, 1);
  gpio_set_dir(KBD_PIN_SCL, GPIO_OUT);
  sleep_us(50);

  // 9 clock pulses — clocks out any partial byte in the STM32's shift register.
  for (int i = 0; i < 9; i++) {
    gpio_put(KBD_PIN_SCL, 0);
    sleep_us(50);
    gpio_put(KBD_PIN_SCL, 1);
    sleep_us(50);
  }

  // If SDA is still stuck low after clocking, issue an explicit STOP.
  // CRITICAL: SCL must go LOW before SDA goes LOW — SDA falling while SCL
  // is HIGH generates a START condition, which would confuse the STM32.
  if (!gpio_get(KBD_PIN_SDA)) {
    gpio_set_dir(KBD_PIN_SDA, GPIO_OUT);
    gpio_put(KBD_PIN_SCL, 0);
    sleep_us(50); // SCL low first
    gpio_put(KBD_PIN_SDA, 0);
    sleep_us(50); // SDA low (SCL is low — no START)
    gpio_put(KBD_PIN_SCL, 1);
    sleep_us(50); // SCL high
    gpio_put(KBD_PIN_SDA, 1);
    sleep_us(50); // SDA high while SCL high → STOP
    gpio_set_dir(KBD_PIN_SDA, GPIO_IN);
    gpio_pull_up(KBD_PIN_SDA);
  }

  // Re-initialize the I2C peripheral and restore GPIO functions.
  i2c_init(KBD_I2C_PORT, KBD_I2C_BAUD);
  gpio_set_function(KBD_PIN_SDA, GPIO_FUNC_I2C);
  gpio_set_function(KBD_PIN_SCL, GPIO_FUNC_I2C);
  gpio_pull_up(KBD_PIN_SDA);
  gpio_pull_up(KBD_PIN_SCL);
}

// Write register address (with STOP), wait, then read `len` bytes.
// The STM32 does NOT support repeated-start — nostop must be false.
// On any failure, calls kbd_recover_i2c_bus() so the STM32 is not left
// mid-transaction (which would cause a Repeated START corruption on the next
// kbd_poll() call).
static bool i2c_read_reg(uint8_t reg, uint8_t *buf, size_t len,
                         uint32_t delay_ms) {
  int ret = i2c_write_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR, &reg, 1, false,
                                 KBD_I2C_TIMEOUT_US);
  if (ret != 1) {
    kbd_recover_i2c_bus();
    return false;
  }
  sleep_ms(delay_ms);
  ret = i2c_read_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR, buf, len, false,
                            KBD_I2C_TIMEOUT_US);
  if (ret != (int)len) {
    // Write succeeded — STM32 prepared its response and is waiting for us to
    // read it. Aborting without a STOP leaves the STM32 mid-transaction.
    // The 9-clock recovery clocks out the waiting bytes and issues a STOP.
    kbd_recover_i2c_bus();
    return false;
  }
  return true;
}

// Write a value to a register (reg address OR'd with WRITE_MASK).
static bool i2c_write_reg(uint8_t reg, uint8_t val) {
  uint8_t buf[2] = {(uint8_t)(reg | KBD_WRITE_MASK), val};
  int ret = i2c_write_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR, buf, 2, false,
                                 KBD_I2C_TIMEOUT_US);
  return ret == 2;
}

bool kbd_init(void) {
  // ── Step 1: Unconditional bus clear ───────────────────────────────────────
  // Sample SDA before recovery so we can log whether the bus was already stuck.
  gpio_init(KBD_PIN_SDA);
  gpio_set_dir(KBD_PIN_SDA, GPIO_IN);
  gpio_pull_up(KBD_PIN_SDA);
  sleep_us(200);

  bool sda_stuck = !gpio_get(KBD_PIN_SDA);
  printf("[KBD] SDA=GP%d before init: %s\n", KBD_PIN_SDA,
         sda_stuck ? "LOW (bus stuck)" : "HIGH (idle)");

  // ── Step 2: Run 9-clock recovery + I2C re-init ────────────────────────────
  kbd_recover_i2c_bus();

  // ── Step 3: Wait for STM32 keyboard scanning to start ─────────────────────
  // The STM32's I2C peripheral starts ~100ms after power-on, but its keyboard
  // FIFO scanning doesn't start until ~2.5s from power-on. If we poll before
  // scanning is active, I2C ACKs but the FIFO is always empty (keys don't
  // work). From observation, 2.5s from RP2350 boot is reliable.
  {
    uint32_t boot_ms = to_ms_since_boot(get_absolute_time());
    if (boot_ms < 2500) {
      printf("[KBD] boot=%lums — waiting %lums for STM32 keyboard scanning\n",
             (unsigned long)boot_ms, (unsigned long)(2500 - boot_ms));
      sleep_ms(2500 - boot_ms);
    }
  }

  // ── Step 4: Poll for STM32 presence (up to 5 seconds) ───────────────────
  printf("[KBD] polling 0x%02X on I2C%d at %dkHz...\n", KBD_I2C_ADDR,
         KBD_I2C_PORT == i2c0 ? 0 : 1, KBD_I2C_BAUD / 1000);

  uint32_t start_ms = to_ms_since_boot(get_absolute_time());
  bool ok = false;
  uint8_t ver = 0;

  for (int poll = 0; poll < 50; poll++) {
    sleep_ms(100);

    uint8_t reg = 0x01;
    int wret = i2c_write_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR, &reg, 1, false,
                                    KBD_I2C_TIMEOUT_US);
    uint32_t t = to_ms_since_boot(get_absolute_time()) - start_ms;

    if (wret == 1) {
      sleep_ms(KBD_REG_DELAY_MS);
      int rret = i2c_read_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR, &ver, 1, false,
                                     KBD_I2C_TIMEOUT_US);
      printf("[KBD] t+%lums: write OK, read ret=%d ver=0x%02X\n",
             (unsigned long)t, rret, ver);
      if (rret == 1) {
        ok = true;
        break;
      }
    } else {
      if (poll == 0 || poll % 10 == 9)
        printf("[KBD] t+%lums: NACK (ret=%d)\n", (unsigned long)t, wret);
    }
  }

  if (ok) {
    printf("[KBD] init OK — I2C%d SDA=GP%d SCL=GP%d fw=0x%02X\n",
           KBD_I2C_PORT == i2c0 ? 0 : 1, KBD_PIN_SDA, KBD_PIN_SCL, ver);
  } else {
    printf("[KBD] FAILED — STM32 never responded in 5s\n");
  }

  return ok;
}

void kbd_poll(void) {
  s_buttons_prev = s_buttons_curr;
  s_last_char = 0;
  s_last_raw_key = 0;

  // Poll REG_FIF (0x09) directly — up to 8 events per frame.
  // Each read returns 2 bytes: [state, keycode].
  // Loop ends when state==IDLE (no more queued events).
  bool poll_ok = false;
  for (int i = 0; i < 8; i++) {
    uint8_t event[2] = {0, 0};
    if (!i2c_read_reg(KBD_REG_FIF, event, 2, KBD_REG_DELAY_MS))
      break;
    poll_ok = true;

    uint8_t state = event[0];
    uint8_t keycode = event[1];

    if (state == KEY_STATE_IDLE)
      break; // FIFO empty

#ifdef KBD_DEBUG
    const char *state_str = state == KEY_STATE_PRESSED    ? "PRESS"
                            : state == KEY_STATE_HOLD     ? "HOLD"
                            : state == KEY_STATE_RELEASED ? "RELEASE"
                                                          : "?";
    if (keycode >= 0x20 && keycode < 0x7F)
      printf("[KBD] %s 0x%02X ('%c')\n", state_str, keycode, keycode);
    else
      printf("[KBD] %s 0x%02X\n", state_str, keycode);
#endif

    bool press = (state == KEY_STATE_PRESSED || state == KEY_STATE_HOLD);
    bool release = (state == KEY_STATE_RELEASED);

    uint32_t btn_flag = 0;
    switch (keycode) {
    case KEY_UP:
      btn_flag = BTN_UP;
      break;
    case KEY_DOWN:
      btn_flag = BTN_DOWN;
      break;
    case KEY_LEFT:
      btn_flag = BTN_LEFT;
      break;
    case KEY_RIGHT:
      btn_flag = BTN_RIGHT;
      break;
    case KEY_ENTER:
      btn_flag = BTN_ENTER;
      break;
    case KEY_ESC:
      btn_flag = BTN_ESC;
      break;
    case KEY_F1:
      btn_flag = BTN_F1;
      break;
    case KEY_F2:
      btn_flag = BTN_F2;
      break;
    case KEY_F3:
      btn_flag = BTN_F3;
      break;
    case KEY_F4:
      btn_flag = BTN_F4;
      break;
    case KEY_F5:
      btn_flag = BTN_F5;
      break;
    case KEY_F6:
      btn_flag = BTN_F6;
      break;
    case KEY_F7:
      btn_flag = BTN_F7;
      break;
    case KEY_F8:
      btn_flag = BTN_F8;
      break;
    case KEY_F9:
      btn_flag = BTN_F9;
      break;
    case KEY_F10:
      btn_flag = BTN_MENU;
      break;
    case KEY_BKSPC:
      btn_flag = BTN_BACKSPACE;
      break;
    case KEY_TAB:
      btn_flag = BTN_TAB;
      break;
    case KEY_MOD_SHL:
      btn_flag = BTN_SHIFT;
      break;
    case KEY_MOD_SHR:
      btn_flag = BTN_SHIFT;
      break;
    case KEY_MOD_CTRL:
      btn_flag = BTN_CTRL;
      break;
    case KEY_MOD_ALT:
      btn_flag = BTN_ALT;
      break;
    case KEY_MOD_SYM:
      btn_flag = BTN_FN;
      break;
    default:
      break;
    }

    if (btn_flag) {
      if (press)
        s_buttons_curr |= btn_flag;
      if (release)
        s_buttons_curr &= ~btn_flag;
    }

    if (press) {
      s_last_raw_key = keycode;
      if (keycode >= 0x20 && keycode < 0x7F)
        s_last_char = (char)keycode;
      if (keycode == KEY_BKSPC)
        s_last_char = (char)KEY_BKSPC;
    }
  }

  // Track consecutive I2C failures for diagnostics.
  if (!poll_ok) {
    s_i2c_fail_count++;
    if (s_i2c_fail_count == 3)
      printf("[KBD] warning: %d consecutive I2C failures — bus recovery active\n",
             s_i2c_fail_count);
  } else {
    s_i2c_fail_count = 0;
  }

  // Intercept BTN_MENU: detect rising edge, flag it for the OS, hide from apps.
  if ((s_buttons_curr & BTN_MENU) && !(s_buttons_prev & BTN_MENU))
    s_menu_pressed = true;
  s_buttons_curr &= ~BTN_MENU;

  // Intercept KEY_BRK (0xD0): flag for screenshot, never reaches apps.
  if (s_last_raw_key == KEY_BRK)
    s_screenshot_pressed = true;
}

char kbd_get_char(void) { return s_last_char; }

uint8_t kbd_get_raw_key(void) { return s_last_raw_key; }

uint32_t kbd_get_buttons(void) { return s_buttons_curr; }

uint32_t kbd_get_buttons_pressed(void) {
  return (s_buttons_curr & ~s_buttons_prev);
}

uint32_t kbd_get_buttons_released(void) {
  return (~s_buttons_curr & s_buttons_prev);
}

int kbd_get_battery_percent(void) {
  static int s_cached_val = -1;
  static uint32_t s_last_ms = 0;
  uint32_t now = to_ms_since_boot(get_absolute_time());

  if (s_buttons_curr != 0 && s_cached_val != -1) {
    return s_cached_val;
  }

  if (s_last_ms == 0 || now - s_last_ms >= 5000) {
    uint8_t val[2] = {0, 0}; // STM32 I2C firmware preps 2 bytes
    if (!i2c_read_reg(KBD_REG_BAT, val, 2, KBD_REG_DELAY_MS))
      return s_cached_val;
    s_cached_val = (int)(val[1] & 0x7F);
    s_last_ms = now;
  }
  return s_cached_val;
}

void kbd_set_backlight(uint8_t brightness) {
  i2c_write_reg(KBD_REG_BL, brightness);
}

bool kbd_consume_menu_press(void) {
  bool val = s_menu_pressed;
  s_menu_pressed = false;
  return val;
}

bool kbd_consume_screenshot_press(void) {
  bool val = s_screenshot_pressed;
  s_screenshot_pressed = false;
  return val;
}

void kbd_clear_state(void) {
  s_buttons_prev = 0;
  s_buttons_curr = 0;
  s_last_char = 0;
  s_last_raw_key = 0;
}
