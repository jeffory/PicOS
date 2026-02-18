#include "keyboard.h"
#include "../hardware.h"
#include "../os/os.h"

#include "pico/stdlib.h"
#include "hardware/i2c.h"
#include "hardware/gpio.h"

#include <stdio.h>

// Key state values from the STM32 firmware (fifo_item.state)
#define KEY_STATE_IDLE     0
#define KEY_STATE_PRESSED  1
#define KEY_STATE_HOLD     2
#define KEY_STATE_RELEASED 3

// The STM32 uses a STOP-based protocol (not repeated-start):
//   1. Write register address as a complete transaction (nostop=false)
//   2. Wait for the STM32 to prepare its response
//   3. Read in a separate transaction
// pelrun/uf2loader used sleep_ms(16), but that's too slow for 60fps apps.
// Testing shows 1ms is reliable and gives us ~60 FPS.
#define KBD_REG_DELAY_MS   1
#define KBD_I2C_TIMEOUT_US 50000

// ── Internal state ────────────────────────────────────────────────────────────

static uint32_t s_buttons_prev = 0;
static uint32_t s_buttons_curr = 0;
static char     s_last_char    = 0;
static uint8_t  s_last_raw_key = 0;  // raw keycode of last press this frame (0 = none)

// ── I2C helpers ───────────────────────────────────────────────────────────────

// Write register address (with STOP), wait, then read `len` bytes.
// The STM32 does NOT support repeated-start — nostop must be false.
static bool i2c_read_reg(uint8_t reg, uint8_t *buf, size_t len) {
    int ret = i2c_write_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR,
                                   &reg, 1, false, KBD_I2C_TIMEOUT_US);
    if (ret != 1) return false;
    sleep_ms(KBD_REG_DELAY_MS);
    ret = i2c_read_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR,
                              buf, len, false, KBD_I2C_TIMEOUT_US);
    return ret == (int)len;
}

// Write a value to a register (reg address OR'd with WRITE_MASK).
static bool i2c_write_reg(uint8_t reg, uint8_t val) {
    uint8_t buf[2] = { (uint8_t)(reg | KBD_WRITE_MASK), val };
    int ret = i2c_write_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR,
                                   buf, 2, false, KBD_I2C_TIMEOUT_US);
    return ret == 2;
}

// ── Public API ────────────────────────────────────────────────────────────────

void kbd_init(void) {
    // ── Step 1: Unconditional bus clear ───────────────────────────────────────
    // Pulse SCL 9 times with SDA as a floating input (pulled high).
    // This clocks through any partial byte the STM32 may be stuck in from
    // a previous aborted transaction.  Because SDA stays high throughout,
    // no I2C START condition is generated — it is safe to do unconditionally.
    gpio_init(KBD_PIN_SDA);
    gpio_set_dir(KBD_PIN_SDA, GPIO_IN);
    gpio_pull_up(KBD_PIN_SDA);
    sleep_us(200);  // let pull-up settle

    bool sda_stuck = !gpio_get(KBD_PIN_SDA);
    printf("[KBD] SDA=GP%d before init: %s\n",
           KBD_PIN_SDA, sda_stuck ? "LOW (bus stuck)" : "HIGH (idle)");

    gpio_init(KBD_PIN_SCL);
    gpio_put(KBD_PIN_SCL, 1);             // pre-load HIGH before driving output
    gpio_set_dir(KBD_PIN_SCL, GPIO_OUT);
    sleep_us(50);

    for (int i = 0; i < 9; i++) {
        gpio_put(KBD_PIN_SCL, 0); sleep_us(50);
        gpio_put(KBD_PIN_SCL, 1); sleep_us(50);
    }

    // If SDA is still stuck low after clocking, issue an explicit STOP.
    // CRITICAL: SCL must go LOW before SDA goes LOW — SDA falling while SCL
    // is HIGH generates a START condition, which would confuse the STM32.
    if (!gpio_get(KBD_PIN_SDA)) {
        gpio_set_dir(KBD_PIN_SDA, GPIO_OUT);
        gpio_put(KBD_PIN_SCL, 0); sleep_us(50);   // SCL low first
        gpio_put(KBD_PIN_SDA, 0); sleep_us(50);   // SDA low (SCL is low — no START)
        gpio_put(KBD_PIN_SCL, 1); sleep_us(50);   // SCL high
        gpio_put(KBD_PIN_SDA, 1); sleep_us(50);   // SDA high while SCL high → STOP
        gpio_set_dir(KBD_PIN_SDA, GPIO_IN);
        gpio_pull_up(KBD_PIN_SDA);
    }

    // ── Step 2: Hand GPIO to the I2C peripheral ───────────────────────────────
    i2c_init(KBD_I2C_PORT, KBD_I2C_BAUD);
    gpio_set_function(KBD_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(KBD_PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(KBD_PIN_SDA);
    gpio_pull_up(KBD_PIN_SCL);

    // ── Step 3: Wait for STM32 keyboard scanning to start ─────────────────────
    // The STM32's I2C peripheral starts ~100ms after power-on, but its keyboard
    // FIFO scanning doesn't start until ~2.5s from power-on. If we poll before
    // scanning is active, I2C ACKs but the FIFO is always empty (keys don't work).
    // From observation, 2.5s from RP2350 boot is reliable.
    {
        uint32_t boot_ms = to_ms_since_boot(get_absolute_time());
        if (boot_ms < 2500) {
            printf("[KBD] boot=%lums — waiting %lums for STM32 keyboard scanning\n",
                   (unsigned long)boot_ms, (unsigned long)(2500 - boot_ms));
            sleep_ms(2500 - boot_ms);
        }
    }

    // ── Step 4: Poll for STM32 presence (up to 5 seconds) ────────────────────
    printf("[KBD] polling 0x%02X on I2C%d at %dkHz...\n",
           KBD_I2C_ADDR, KBD_I2C_PORT == i2c0 ? 0 : 1, KBD_I2C_BAUD / 1000);

    uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    bool ok = false;
    uint8_t ver = 0;

    for (int poll = 0; poll < 50; poll++) {
        sleep_ms(100);

        uint8_t reg = 0x01;
        int wret = i2c_write_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR,
                                        &reg, 1, false, KBD_I2C_TIMEOUT_US);
        uint32_t t = to_ms_since_boot(get_absolute_time()) - start_ms;

        if (wret == 1) {
            sleep_ms(KBD_REG_DELAY_MS);
            int rret = i2c_read_timeout_us(KBD_I2C_PORT, KBD_I2C_ADDR,
                                           &ver, 1, false, KBD_I2C_TIMEOUT_US);
            printf("[KBD] t+%lums: write OK, read ret=%d ver=0x%02X\n",
                   (unsigned long)t, rret, ver);
            if (rret == 1) { ok = true; break; }
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
}

void kbd_poll(void) {
    s_buttons_prev = s_buttons_curr;
    s_last_char    = 0;
    s_last_raw_key = 0;

    // Poll REG_FIF (0x09) directly — up to 8 events per frame.
    // Each read returns 2 bytes: [state, keycode].
    // Loop ends when state==IDLE (no more queued events).
    for (int i = 0; i < 8; i++) {
        uint8_t event[2] = { 0, 0 };
        if (!i2c_read_reg(KBD_REG_FIF, event, 2)) break;

        uint8_t state   = event[0];
        uint8_t keycode = event[1];

        if (state == KEY_STATE_IDLE) break;   // FIFO empty

        const char *state_str = state == KEY_STATE_PRESSED  ? "PRESS"   :
                                state == KEY_STATE_HOLD     ? "HOLD"    :
                                state == KEY_STATE_RELEASED ? "RELEASE" : "?";
        if (keycode >= 0x20 && keycode < 0x7F)
            printf("[KBD] %s 0x%02X ('%c')\n", state_str, keycode, keycode);
        else
            printf("[KBD] %s 0x%02X\n", state_str, keycode);

        bool press   = (state == KEY_STATE_PRESSED || state == KEY_STATE_HOLD);
        bool release = (state == KEY_STATE_RELEASED);

        uint32_t btn_flag = 0;
        switch (keycode) {
            case KEY_UP:    btn_flag = BTN_UP;    break;
            case KEY_DOWN:  btn_flag = BTN_DOWN;  break;
            case KEY_LEFT:  btn_flag = BTN_LEFT;  break;
            case KEY_RIGHT: btn_flag = BTN_RIGHT; break;
            case KEY_ENTER: btn_flag = BTN_ENTER;  break;
            case KEY_ESC:   btn_flag = BTN_ESC;    break;
            case KEY_MENU:  btn_flag = BTN_MENU;  break;
            default: break;
        }

        if (btn_flag) {
            if (press)   s_buttons_curr |=  btn_flag;
            if (release) s_buttons_curr &= ~btn_flag;
        }

        if (press) {
            s_last_raw_key = keycode;
            if (keycode >= 0x20 && keycode < 0x7F)
                s_last_char = (char)keycode;
            if (keycode == KEY_BKSPC)
                s_last_char = (char)KEY_BKSPC;
        }
    }
}

char kbd_get_char(void) {
    return s_last_char;
}

uint8_t kbd_get_raw_key(void) {
    return s_last_raw_key;
}

uint32_t kbd_get_buttons(void) {
    return s_buttons_curr;
}

uint32_t kbd_get_buttons_pressed(void) {
    return (s_buttons_curr & ~s_buttons_prev);
}

uint32_t kbd_get_buttons_released(void) {
    return (~s_buttons_curr & s_buttons_prev);
}

int kbd_get_battery_percent(void) {
    uint8_t val = 0;
    if (!i2c_read_reg(KBD_REG_BAT, &val, 1)) return -1;
    return (int)(val & 0x7F);   // bit 7 = charging flag
}

void kbd_set_backlight(uint8_t brightness) {
    i2c_write_reg(KBD_REG_BL, brightness);
}
