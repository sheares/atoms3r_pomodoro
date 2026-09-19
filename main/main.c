#include "gc9107.h"
#include "lp5562.h"
#include "bmi270.h"
#include "font.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"

static const char *TAG = "pomodoro";

#define BTN_GPIO         41
#define TOTAL_SECONDS    (8 * 60)     /* 8 minutes */
#define FRAME_PERIOD_MS  33           /* ~30 FPS */

/* ── State ─────────────────────────────────────────────────────────────────── */
static int     s_remaining     = TOTAL_SECONDS;   /* seconds left */
static int64_t s_accum_us      = 0;               /* fractional seconds while running */
static bool    s_imu_ok        = false;
static int     s_upright_score = 0;               /* hysteresis: positive → upright */
static int     s_rainbow_frame = 0;

/* ── Palette ───────────────────────────────────────────────────────────────── */
#define C_BG       RGB565( 18,  20,  28)
#define C_CARD     RGB565( 40,  44,  60)
#define C_CARD_LO  RGB565( 24,  26,  38)        /* below the crease line */
#define C_DIGIT    RGB565(245, 245, 250)
#define C_DIM      RGB565(120, 130, 150)
#define C_RUN      RGB565( 90, 210, 130)
#define C_PAUSE    RGB565(255, 175,  60)
#define C_CRACK    RGB565(  6,   8,  12)        /* the dark slit between halves */

/* ── Auto-rotate LCD to match how the device is held ─────────────────────── */
/* Maps the dominant horizontal axis under gravity to one of four 90° rotations.
 * Requires 3 stable polls before switching so a brief wobble doesn't flip it. */
static void poll_orientation(void)
{
    if (!s_imu_ok) return;

    int16_t ax, ay, az;
    if (!bmi270_read_accel(&ax, &ay, &az)) return;

    /* When flat, ax/ay are noisy — keep the last known rotation. */
    if (abs(az) > abs(ax) && abs(az) > abs(ay)) return;

    uint8_t new_rot;
    if (abs(ax) > abs(ay))
        new_rot = (ax > 0) ? 0 : 2;
    else
        new_rot = (ay > 0) ? 3 : 1;

    static uint8_t s_pending = 3;
    static int     s_stable  = 0;
    static uint8_t s_current = 3;

    if (new_rot == s_pending) {
        if (++s_stable >= 3 && new_rot != s_current) {
            s_current = new_rot;
            gc9107_set_rotation(new_rot);
            ESP_LOGI(TAG, "Rotation → %d°", new_rot * 90);
        }
    } else {
        s_pending = new_rot;
        s_stable  = 0;
    }
}

/* ── Upright detection ─────────────────────────────────────────────────────── */
/* Upright = device sitting on an edge, LCD facing the user.
 * In that pose Z is small and X or Y carries the 1 g of gravity. */
static bool poll_upright(void)
{
    if (!s_imu_ok) return true;     /* if IMU isn't there, always run */

    int16_t ax, ay, az;
    if (!bmi270_read_accel(&ax, &ay, &az)) return false;

    int aza = abs(az), axa = abs(ax), aya = abs(ay);
    bool now_upright = (axa > aza) || (aya > aza);

    /* Hysteresis so brief wobbles don't flip the state. */
    if (now_upright) {
        if (s_upright_score < 4) s_upright_score++;
    } else {
        if (s_upright_score > -4) s_upright_score--;
    }
    return s_upright_score > 0;
}

/* ── Timer tick ────────────────────────────────────────────────────────────── */
static void update_timer(bool upright, int64_t now_us, int64_t last_us)
{
    if (!upright)             return;
    if (s_remaining <= 0)     return;
    s_accum_us += (now_us - last_us);
    while (s_accum_us >= 1000000) {
        s_accum_us -= 1000000;
        if (--s_remaining <= 0) {
            s_remaining = 0;
            s_accum_us  = 0;
            return;
        }
    }
}

static void reset_timer(void)
{
    s_remaining     = TOTAL_SECONDS;
    s_accum_us      = 0;
    s_rainbow_frame = 0;
    ESP_LOGI(TAG, "Reset to %02d:%02d", TOTAL_SECONDS / 60, TOTAL_SECONDS % 60);
}

/* ── Flip-card style rendering ─────────────────────────────────────────────── */
/* 4 cards: M M : S S  (the colon is rendered between cards, not a card itself) */
#define CARD_W      26
#define CARD_H      54
#define CARD_GAP     4
#define COLON_W      6
#define CARDS_Y     32

static void draw_card(int x, int digit_or_minus_one)
{
    /* Card body — two halves separated by a thin "crack" */
    int half = CARD_H / 2;
    gc9107_fill_rect(x, CARDS_Y,           CARD_W, half,         C_CARD);
    gc9107_fill_rect(x, CARDS_Y + half,    CARD_W, CARD_H - half, C_CARD_LO);
    gc9107_fill_rect(x, CARDS_Y + half - 1, CARD_W, 2,            C_CRACK);

    if (digit_or_minus_one < 0) return;

    char s[2] = { (char)('0' + digit_or_minus_one), 0 };
    /* font is 5 wide * 8 tall at scale 1. scale 4 → 20×32.
     * Center inside CARD_W=26, CARD_H=54: x_off=3, y_off=11. */
    gc9107_draw_string(x + 3, CARDS_Y + 11, s, C_DIGIT, C_CARD, 4);
    /* Re-stamp the lower-half background colour on the bottom of the digit so
     * the digit visually straddles the crease without painting over it. */
    /* (font is already painted; nothing to do — the crease is drawn last below). */
    gc9107_fill_rect(x, CARDS_Y + half - 1, CARD_W, 2, C_CRACK);
}

static void draw_colon(int x)
{
    int half = CARD_H / 2;
    /* Dot pair, vertically arranged within the colon column */
    int dy1 = CARDS_Y + half / 2 - 2;
    int dy2 = CARDS_Y + half + half / 2 - 2;
    gc9107_fill_rect(x + 1, dy1, 4, 4, C_DIGIT);
    gc9107_fill_rect(x + 1, dy2, 4, 4, C_DIGIT);
}

static void draw_timer(void)
{
    int m = s_remaining / 60;
    int s = s_remaining % 60;

    int total_w = 4 * CARD_W + 3 * CARD_GAP + COLON_W + CARD_GAP;
    int x = (LCD_WIDTH - total_w) / 2;

    draw_card(x, m / 10);          x += CARD_W + CARD_GAP;
    draw_card(x, m % 10);          x += CARD_W + CARD_GAP;
    draw_colon(x);                 x += COLON_W + CARD_GAP;
    draw_card(x, s / 10);          x += CARD_W + CARD_GAP;
    draw_card(x, s % 10);
}

static void draw_centred(int y, const char *text, uint16_t fg, uint16_t bg, uint8_t scale)
{
    int w = (int)strlen(text) * 6 * scale;
    int x = (LCD_WIDTH - w) / 2;
    if (x < 0) x = 0;
    gc9107_draw_string(x, y, text, fg, bg, scale);
}

/* ── Rainbow ───────────────────────────────────────────────────────────────── */
/* HSV → RGB565, hue in [0, 360). */
static uint16_t hsv565(float h)
{
    h = fmodf(h, 360.0f);
    if (h < 0) h += 360.0f;
    float c = 1.0f;
    float x = 1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f);
    float r = 0, g = 0, b = 0;
    if      (h < 60)  { r = c; g = x; }
    else if (h < 120) { r = x; g = c; }
    else if (h < 180) { g = c; b = x; }
    else if (h < 240) { g = x; b = c; }
    else if (h < 300) { r = x; b = c; }
    else              { r = c; b = x; }
    return RGB565((uint8_t)(r * 255), (uint8_t)(g * 255), (uint8_t)(b * 255));
}

/* Precomputed full-cycle hue LUT: 256 entries spanning HSV 0–360°. */
static uint16_t s_rainbow_lut[256];
static void init_rainbow_lut(void)
{
    for (int i = 0; i < 256; i++) {
        s_rainbow_lut[i] = hsv565((float)i * 360.0f / 256.0f);
    }
}

/* Transparent text overlay — only the glyph's foreground pixels are drawn,
 * so the rainbow shows through where the glyph has no bits set. */
static void draw_text_overlay(int x, int y, const char *str, uint16_t fg, uint8_t scale)
{
    while (*str) {
        char c = *str++;
        if (c < FONT_FIRST || c > FONT_LAST) c = '?';
        const uint8_t *glyph = font5x8[c - FONT_FIRST];
        for (int col = 0; col < FONT_WIDTH; col++) {
            uint8_t col_data = glyph[col];
            for (int row = 0; row < FONT_HEIGHT; row++) {
                if (col_data & (1 << row)) {
                    if (scale == 1) gc9107_draw_pixel(x + col, y + row, fg);
                    else gc9107_fill_rect(x + col * scale, y + row * scale, scale, scale, fg);
                }
            }
        }
        x += (FONT_WIDTH + 1) * scale;
    }
}

static void draw_rainbow(void)
{
    /* Per-pixel diagonal rainbow via LUT — smooth, no blockiness.
     * hue index = (x + y + phase) wrapped to 0..255 so it tiles seamlessly. */
    const int phase = s_rainbow_frame * 2;
    for (int y = 0; y < LCD_HEIGHT; y++) {
        for (int x = 0; x < LCD_WIDTH; x++) {
            gc9107_draw_pixel(x, y, s_rainbow_lut[(x + y + phase) & 0xFF]);
        }
    }
    /* "DONE!" inside a flip-card matching the timer's look. */
    const char *msg = "DONE!";
    int text_w = 5 * 6 * 3;          /* 5 chars × (5+1) × scale 3 = 90 */
    int text_h = 8 * 3;              /* font height × scale = 24 */
    int card_w = text_w + 12;        /* 6 px H padding */
    int card_h = text_h + 18;        /* 9 px V padding */
    int card_x = (LCD_WIDTH  - card_w) / 2;
    int card_y = (LCD_HEIGHT - card_h) / 2;
    int half   = card_h / 2;

    gc9107_fill_rect(card_x, card_y,            card_w, half,           C_CARD);
    gc9107_fill_rect(card_x, card_y + half,     card_w, card_h - half,  C_CARD_LO);
    gc9107_fill_rect(card_x, card_y + half - 1, card_w, 2,              C_CRACK);

    int text_x = card_x + (card_w - text_w) / 2;
    int text_y = card_y + (card_h - text_h) / 2;
    draw_text_overlay(text_x, text_y, msg, C_DIGIT, 3);
}

/* ── Main loop ─────────────────────────────────────────────────────────────── */
void app_main(void)
{
    gc9107_init();
    gc9107_set_rotation(3);             /* AtomS3R upright default */
    init_rainbow_lut();

    i2c_master_bus_handle_t sys_bus = NULL;
    if (lp5562_init(&sys_bus)) {
        lp5562_set_backlight(sys_bus, 200);
    }
    s_imu_ok = sys_bus && bmi270_init(sys_bus);
    if (!s_imu_ok) ESP_LOGW(TAG, "BMI270 not found — timer will run continuously");

    gpio_config_t btn = {
        .pin_bit_mask = 1ULL << BTN_GPIO,
        .mode         = GPIO_MODE_INPUT,
        .pull_up_en   = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type    = GPIO_INTR_DISABLE,
    };
    gpio_config(&btn);

    int  btn_debounce  = 0;
    bool btn_triggered = false;

    int64_t last_us = esp_timer_get_time();

    for (;;) {
        /* ── Button: any press resets the timer. ───────────────────── */
        int lvl = gpio_get_level(BTN_GPIO);
        if (lvl == 0) {
            if (btn_debounce < 3) btn_debounce++;
            if (btn_debounce == 3 && !btn_triggered) {
                reset_timer();
                btn_triggered = true;
            }
        } else {
            btn_debounce  = 0;
            btn_triggered = false;
        }

        /* ── IMU + timer ───────────────────────────────────────────── */
        bool upright = poll_upright();
        int64_t now_us = esp_timer_get_time();
        update_timer(upright, now_us, last_us);
        last_us = now_us;

        /* Re-check display rotation every ~10 frames (≈ 330 ms). */
        static int orient_tick = 0;
        if (++orient_tick >= 10) {
            orient_tick = 0;
            poll_orientation();
        }

        /* ── Render ────────────────────────────────────────────────── */
        if (s_remaining == 0) {
            s_rainbow_frame++;
            draw_rainbow();
        } else {
            gc9107_fill_screen(C_BG);
            draw_centred(8, "FOCUS", C_DIM, C_BG, 1);
            draw_timer();
            if (upright) draw_centred(100, "running",  C_RUN,   C_BG, 1);
            else         draw_centred(100, "paused",   C_PAUSE, C_BG, 1);
            draw_centred(116, "press = reset", C_DIM, C_BG, 1);
        }
        gc9107_flush();

        vTaskDelay(pdMS_TO_TICKS(FRAME_PERIOD_MS));
    }
}
