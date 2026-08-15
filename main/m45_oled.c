#include "m45_oled.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bitaxe_hw.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_ssd1306.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "i2c_bitaxe.h"
#include "m45_config.h"
#include "qrcode.h"
#include "stratum_minimal.h"
#include "wifi_http.h"

#if CONFIG_M45_BITAXE_OLED_ENABLE

#define OLED_WIDTH CONFIG_M45_BITAXE_OLED_WIDTH
#define OLED_HEIGHT CONFIG_M45_BITAXE_OLED_HEIGHT
#if (OLED_HEIGHT % 8) != 0
#error "M45_BITAXE_OLED_HEIGHT must be a multiple of 8"
#endif
#define OLED_PAGE_COUNT (OLED_HEIGHT / 8)
#define OLED_FRAME_BYTES (OLED_WIDTH * OLED_PAGE_COUNT)
#define FONT_WIDTH 4
#define FONT_HEIGHT 6
#define FONT_ADVANCE 5
#define OLED_TEXT_Y_OFFSET 1
#define QR_MAX_VERSION 3
#define QR_MAX_MODULES ((4 * QR_MAX_VERSION) + 17)
#define QR_MAX_BUFFER_SIZE (((QR_MAX_MODULES * QR_MAX_MODULES) + 7) / 8)
#define QR_ECC_LEVEL ECC_MEDIUM
#define QR_TEXT_GAP 3
#define OLED_ALERT_REFRESH_MS 100
#define OLED_STATUS_REFRESH_MS 1000
#define OLED_MINING_REFRESH_MS 4000
#define OLED_RECOVERY_HOLD_MS 5000
#define OLED_RECOVERY_POLL_MS 50
#ifndef CONFIG_GPIO_BUTTON_BOOT
#define CONFIG_GPIO_BUTTON_BOOT 0
#endif

static const char *TAG = "m45_oled";
static esp_lcd_panel_handle_t g_panel;
static uint8_t g_frame[OLED_FRAME_BYTES];
static uint8_t g_previous_frame[OLED_FRAME_BYTES];
static bool g_full_refresh = true;
static bool g_oled_inverted;
static bool g_oled_sleeping;
static bool g_button_was_pressed;
static bool g_runtime_inverted;
static bool g_runtime_qr_on_right;
static bool g_runtime_swap_sides_on_next_block;
static bool g_factory_reset_view;
static uint32_t g_last_runtime_block_seq;
static TickType_t g_last_oled_input_tick;

static const uint8_t *glyph4x6_rows(char ch)
{
    static const uint8_t blank[FONT_HEIGHT] = {0};
    static const uint8_t digits[10][FONT_HEIGHT] = {
        {6, 9, 9, 9, 9, 6},   {2, 6, 2, 2, 2, 7},   {6, 9, 1, 2, 4, 15},
        {14, 1, 6, 1, 9, 6},  {2, 6, 10, 15, 2, 2}, {15, 8, 14, 1, 9, 6},
        {6, 8, 14, 9, 9, 6},  {15, 1, 2, 4, 4, 4},  {6, 9, 6, 9, 9, 6},
        {6, 9, 9, 7, 1, 6},
    };
    static const uint8_t letters[26][FONT_HEIGHT] = {
        {6, 9, 9, 15, 9, 9},   {14, 9, 14, 9, 9, 14}, {7, 8, 8, 8, 8, 7},
        {14, 9, 9, 9, 9, 14},  {15, 8, 14, 8, 8, 15}, {15, 8, 14, 8, 8, 8},
        {7, 8, 8, 11, 9, 7},   {9, 9, 15, 9, 9, 9},   {7, 2, 2, 2, 2, 7},
        {1, 1, 1, 1, 9, 6},    {9, 10, 12, 10, 9, 9}, {8, 8, 8, 8, 8, 15},
        {9, 15, 15, 9, 9, 9},  {9, 13, 15, 11, 9, 9}, {6, 9, 9, 9, 9, 6},
        {14, 9, 9, 14, 8, 8},  {6, 9, 9, 9, 11, 7},   {14, 9, 9, 14, 10, 9},
        {7, 8, 6, 1, 1, 14},   {15, 2, 2, 2, 2, 2},   {9, 9, 9, 9, 9, 6},
        {9, 9, 9, 9, 6, 6},    {9, 9, 9, 15, 15, 9},  {9, 9, 6, 6, 9, 9},
        {9, 9, 6, 2, 2, 2},    {15, 1, 2, 4, 8, 15},
    };
    static const uint8_t glyph_dash[FONT_HEIGHT] = {0, 0, 0, 15, 0, 0};
    static const uint8_t glyph_dot[FONT_HEIGHT] = {0, 0, 0, 0, 0, 6};
    static const uint8_t glyph_comma[FONT_HEIGHT] = {0, 0, 0, 0, 2, 4};
    static const uint8_t glyph_colon[FONT_HEIGHT] = {0, 6, 0, 0, 6, 0};
    static const uint8_t glyph_slash[FONT_HEIGHT] = {1, 1, 2, 2, 4, 4};
    static const uint8_t glyph_underscore[FONT_HEIGHT] = {0, 0, 0, 0, 0, 15};
    static const uint8_t glyph_plus[FONT_HEIGHT] = {0, 0, 2, 15, 2, 0};
    static const uint8_t glyph_percent[FONT_HEIGHT] = {9, 1, 2, 8, 9, 2};

    if (ch >= '0' && ch <= '9') {
        return digits[ch - '0'];
    }
    if (ch >= 'a' && ch <= 'z') {
        ch = (char)(ch - 'a' + 'A');
    }
    if (ch >= 'A' && ch <= 'Z') {
        return letters[ch - 'A'];
    }
    switch (ch) {
    case '-':
        return glyph_dash;
    case '.':
        return glyph_dot;
    case ',':
        return glyph_comma;
    case ':':
        return glyph_colon;
    case '/':
        return glyph_slash;
    case '_':
        return glyph_underscore;
    case '+':
        return glyph_plus;
    case '%':
        return glyph_percent;
    default:
        return blank;
    }
}

static void oled_set_pixel(int x, int y)
{
    if (x < 0 || x >= OLED_WIDTH || y < 0 || y >= OLED_HEIGHT) {
        return;
    }
    g_frame[x + (y / 8) * OLED_WIDTH] |= (uint8_t)(1U << (y & 7));
}

static void oled_draw_text(int x, int y, const char *text)
{
    y += OLED_TEXT_Y_OFFSET;
    while (*text != '\0' && x <= OLED_WIDTH - FONT_WIDTH) {
        const uint8_t *rows = glyph4x6_rows(*text++);
        for (int row = 0; row < FONT_HEIGHT; ++row) {
            for (int col = 0; col < FONT_WIDTH; ++col) {
                if ((rows[row] & (1U << (FONT_WIDTH - 1 - col))) != 0) {
                    oled_set_pixel(x + col, y + row);
                }
            }
        }
        x += FONT_ADVANCE;
    }
}

static void oled_draw_text_clipped(int x, int y, const char *text, int max_width)
{
    const int end_x = x + max_width;
    y += OLED_TEXT_Y_OFFSET;
    while (*text != '\0' && x <= end_x - FONT_WIDTH && x <= OLED_WIDTH - FONT_WIDTH) {
        const uint8_t *rows = glyph4x6_rows(*text++);
        for (int row = 0; row < FONT_HEIGHT; ++row) {
            for (int col = 0; col < FONT_WIDTH; ++col) {
                if ((rows[row] & (1U << (FONT_WIDTH - 1 - col))) != 0) {
                    oled_set_pixel(x + col, y + row);
                }
            }
        }
        x += FONT_ADVANCE;
    }
}

static void oled_draw_text_scaled(int x, int y, const char *text, int scale)
{
    y += OLED_TEXT_Y_OFFSET;
    while (*text != '\0' && x <= OLED_WIDTH - (FONT_WIDTH * scale)) {
        const uint8_t *rows = glyph4x6_rows(*text++);
        for (int row = 0; row < FONT_HEIGHT; ++row) {
            for (int col = 0; col < FONT_WIDTH; ++col) {
                if ((rows[row] & (1U << (FONT_WIDTH - 1 - col))) == 0) {
                    continue;
                }
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        oled_set_pixel(x + (col * scale) + dx, y + (row * scale) + dy);
                    }
                }
            }
        }
        x += FONT_ADVANCE * scale;
    }
}

static void oled_draw_centered_scaled(int y, const char *text, int scale)
{
    const int width = (int)strlen(text) * FONT_ADVANCE * scale;
    const int x = width < OLED_WIDTH ? (OLED_WIDTH - width) / 2 : 0;
    oled_draw_text_scaled(x, y, text, scale);
}

static void oled_set_invert(bool invert)
{
    if (g_panel == NULL || g_oled_sleeping || g_oled_inverted == invert) {
        return;
    }
    if (esp_lcd_panel_invert_color(g_panel, invert) == ESP_OK) {
        g_oled_inverted = invert;
    }
}

static void oled_flush(void)
{
    if (g_panel == NULL || g_oled_sleeping) {
        return;
    }
    if (g_full_refresh || memcmp(g_frame, g_previous_frame, sizeof(g_frame)) != 0) {
        if (esp_lcd_panel_draw_bitmap(g_panel, 0, 0, OLED_WIDTH, OLED_HEIGHT, g_frame) == ESP_OK) {
            memcpy(g_previous_frame, g_frame, sizeof(g_previous_frame));
            g_full_refresh = false;
        }
    }
}

static void oled_draw_line(int row, const char *text)
{
    oled_draw_text(0, row * 8, text);
}

static void oled_add_line(int *row, const char *text)
{
    if (*row >= OLED_PAGE_COUNT || text == NULL || text[0] == '\0') {
        return;
    }
    oled_draw_line(*row, text);
    ++(*row);
}

static bool oled_draw_qr_payload(const char *payload, int x, int y, int max_size)
{
    QRCode qr;
    uint8_t data[QR_MAX_BUFFER_SIZE];
    uint8_t version = 0;
    int scale = 0;

    for (uint8_t candidate = 1; candidate <= QR_MAX_VERSION; ++candidate) {
        memset(data, 0, sizeof(data));
        if (qrcode_initText(&qr, data, candidate, QR_ECC_LEVEL, payload) == 0 &&
            qr.size <= max_size) {
            version = candidate;
            scale = max_size / qr.size;
            break;
        }
    }
    if (version == 0 || scale <= 0) {
        oled_draw_text_clipped(x, y, "QR ERR", max_size);
        return false;
    }

    for (uint8_t candidate = (uint8_t)(version + 1); candidate <= QR_MAX_VERSION; ++candidate) {
        const int candidate_size = (int)(4 * candidate) + 17;
        if (candidate_size * scale > max_size) {
            break;
        }
        version = candidate;
    }

    memset(data, 0, sizeof(data));
    if (qrcode_initText(&qr, data, version, QR_ECC_LEVEL, payload) != 0) {
        oled_draw_text_clipped(x, y, "QR ERR", max_size);
        return false;
    }

    const int qr_pixels = qr.size * scale;
    const int offset = (max_size - qr_pixels) / 2;

    for (int row = 0; row < qr.size; ++row) {
        for (int col = 0; col < qr.size; ++col) {
            if (!qrcode_getModule(&qr, (uint8_t)col, (uint8_t)row)) {
                continue;
            }
            const int px = x + offset + (col * scale);
            const int py = y + offset + (row * scale);
            for (int dy = 0; dy < scale; ++dy) {
                for (int dx = 0; dx < scale; ++dx) {
                    oled_set_pixel(px + dx, py + dy);
                }
            }
        }
    }
    return true;
}

static void oled_escape_wifi_qr_text(char *dest, size_t dest_len, const char *src)
{
    size_t out = 0;
    if (dest_len == 0) {
        return;
    }
    if (src == NULL) {
        dest[0] = '\0';
        return;
    }

    while (*src != '\0' && out + 1 < dest_len) {
        const bool escape = *src == '\\' || *src == ';' || *src == ',' ||
                            *src == ':' || *src == '"';
        if (escape) {
            if (out + 2 >= dest_len) {
                break;
            }
            dest[out++] = '\\';
        }
        dest[out++] = *src++;
    }
    dest[out] = '\0';
}

static int oled_qr_size(void)
{
    int size = OLED_HEIGHT < OLED_WIDTH ? OLED_HEIGHT : OLED_WIDTH;
    if (size > 64) {
        size = 64;
    }
    return size;
}

static void oled_draw_qr_text_lines(int text_left, int text_width, const char *line1,
                                    const char *line2, const char *line3,
                                    const char *line4)
{
    if (text_width < FONT_WIDTH) {
        return;
    }

    if (OLED_HEIGHT >= 64) {
        oled_draw_text_clipped(text_left, 8, line1, text_width);
        oled_draw_text_clipped(text_left, 20, line2, text_width);
        oled_draw_text_clipped(text_left, 34, line3, text_width);
        oled_draw_text_clipped(text_left, 50, line4, text_width);
    } else {
        oled_draw_text_clipped(text_left, 0, line1, text_width);
        oled_draw_text_clipped(text_left, 8, line2, text_width);
        oled_draw_text_clipped(text_left, 16, line3, text_width);
        oled_draw_text_clipped(text_left, 24, line4, text_width);
    }
}

static void oled_draw_setup_qr_view(void)
{
    char ssid[64];
    char payload[96];
    const int qr_size = oled_qr_size();
    const int text_left = qr_size + QR_TEXT_GAP;

    oled_escape_wifi_qr_text(ssid, sizeof(ssid), wifi_http_setup_ssid());
    snprintf(payload, sizeof(payload), "WIFI:S:%s;;", ssid);
    oled_draw_qr_payload(payload, 0, 0, qr_size);
    oled_draw_qr_text_lines(text_left, OLED_WIDTH - text_left, "SETUP", wifi_http_setup_ssid(),
                            wifi_http_setup_ip(), "SCAN WIFI");
}

static void oled_draw_runtime_qr_lines(const char *line1, const char *line2, const char *line3,
                                       const char *line4, bool qr_on_right)
{
    char payload[48];
    const char *ip = wifi_http_connected() ? wifi_http_ip() : wifi_http_setup_ip();
    const int qr_size = oled_qr_size();
    const int qr_x = qr_on_right ? OLED_WIDTH - qr_size : 0;
    const int text_left = qr_on_right ? 0 : qr_size + QR_TEXT_GAP;
    const int text_width = qr_on_right ? qr_x - QR_TEXT_GAP : OLED_WIDTH - text_left;

    snprintf(payload, sizeof(payload), "HTTP://%s", ip != NULL && ip[0] != '\0' ? ip : "0.0.0.0");
    oled_draw_qr_payload(payload, qr_x, 0, qr_size);
    oled_draw_qr_text_lines(text_left, text_width, line1, line2, line3, line4);
}

static void oled_draw_runtime_qr_view(const char *status)
{
    const char *ip = wifi_http_connected() ? wifi_http_ip() : wifi_http_setup_ip();
    oled_draw_runtime_qr_lines("WEB", ip != NULL && ip[0] != '\0' ? ip : "NO IP",
                               status != NULL ? status : "SCAN", "SCAN QR", false);
}

static void oled_draw_factory_reset_view(void)
{
    const char *text = "Resetting settings";
    const int text_width = ((int)strlen(text) * FONT_ADVANCE) - 1;
    const int x = text_width < OLED_WIDTH ? (OLED_WIDTH - text_width) / 2 : 0;
    const int y = (OLED_HEIGHT - FONT_HEIGHT) / 2;
    oled_draw_text_clipped(x, y, text, OLED_WIDTH - x);
}

static void oled_draw_recovery_view(uint32_t remaining_seconds)
{
    char countdown[24];
    snprintf(countdown, sizeof(countdown), "HOLD %lu SEC TO RESET",
             (unsigned long)remaining_seconds);
    oled_draw_centered_scaled(2, "FULL SETTINGS RESET", 1);
    oled_draw_centered_scaled(12, "RELEASE TO CANCEL", 1);
    oled_draw_centered_scaled(22, countdown, 1);
}

static void draw_block_found_alert(void)
{
    const int scale = OLED_HEIGHT >= 64 ? 4 : 2;
    const int line_height = FONT_HEIGHT * scale;
    const int gap = OLED_HEIGHT >= 64 ? 8 : 4;
    const int total_height = (line_height * 2) + gap;
    const int y = total_height < OLED_HEIGHT ? (OLED_HEIGHT - total_height) / 2 : 0;
    oled_draw_centered_scaled(y, "BLOCK", scale);
    oled_draw_centered_scaled(y + line_height + gap, "FOUND", scale);
}

static bool oled_alert_button_pressed(void)
{
    return gpio_get_level((gpio_num_t)CONFIG_GPIO_BUTTON_BOOT) == 0;
}

static void oled_display_sleep(void)
{
    if (g_panel == NULL || g_oled_sleeping) {
        return;
    }
    if (g_oled_inverted &&
        esp_lcd_panel_invert_color(g_panel, false) == ESP_OK) {
        g_oled_inverted = false;
    }
    if (esp_lcd_panel_disp_on_off(g_panel, false) == ESP_OK) {
        g_oled_sleeping = true;
    }
}

static void oled_display_wake(void)
{
    if (g_panel == NULL || !g_oled_sleeping) {
        return;
    }
    if (esp_lcd_panel_disp_on_off(g_panel, true) == ESP_OK) {
        g_oled_sleeping = false;
        g_full_refresh = true;
        g_oled_inverted = false;
    }
}

static bool oled_record_input(TickType_t now)
{
    const bool was_sleeping = g_oled_sleeping;
    g_last_oled_input_tick = now;
    if (was_sleeping) {
        oled_display_wake();
    }
    return was_sleeping;
}

static void oled_update_sleep_state(TickType_t now, bool block_alert_active,
                                    const m45_config_t *config)
{
    if (block_alert_active) {
        oled_display_wake();
        g_last_oled_input_tick = now;
        return;
    }

    const uint16_t sleep_minutes = config != NULL ? config->display_sleep_minutes
                                                  : M45_DISPLAY_SLEEP_DEFAULT_MINUTES;
    if (sleep_minutes == 0) {
        oled_display_wake();
        g_last_oled_input_tick = now;
        return;
    }

    const uint32_t timeout_ms = (uint32_t)sleep_minutes * 60U * 1000U;
    if (g_last_oled_input_tick == 0) {
        g_last_oled_input_tick = now;
    }
    if (!g_oled_sleeping &&
        (now - g_last_oled_input_tick) >= pdMS_TO_TICKS(timeout_ms)) {
        oled_display_sleep();
    }
}

static bool oled_mining_view_active(GlobalState *state, const stratum_minimal_stats_t *stats)
{
    return state != NULL && !state->SYSTEM_MODULE.hardware_fault && wifi_http_connected() &&
           state->ASIC_initalized && stats->connected;
}

static void oled_reset_runtime_screensaver(bool clear_block)
{
    if (g_runtime_inverted) {
        g_runtime_inverted = false;
    }
    if (g_oled_inverted) {
        oled_set_invert(false);
    }
    if (clear_block) {
        g_last_runtime_block_seq = 0;
        if (g_runtime_qr_on_right) {
            g_full_refresh = true;
        }
        g_runtime_qr_on_right = false;
        g_runtime_swap_sides_on_next_block = false;
    }
}

static void oled_update_screensaver_state(const m45_config_t *config,
                                          const stratum_minimal_stats_t *stats)
{
    if (config == NULL || stats == NULL || !config->display_screensaver_enabled) {
        oled_reset_runtime_screensaver(true);
        return;
    }

    if (stats->current_block_seq != 0 && stats->current_block_seq != g_last_runtime_block_seq) {
        if (g_last_runtime_block_seq != 0) {
            g_runtime_inverted = !g_runtime_inverted;
            if (g_runtime_swap_sides_on_next_block) {
                g_runtime_qr_on_right = !g_runtime_qr_on_right;
                g_full_refresh = true;
            }
            g_runtime_swap_sides_on_next_block = !g_runtime_swap_sides_on_next_block;
        }
        g_last_runtime_block_seq = stats->current_block_seq;
    }

    oled_set_invert(g_runtime_inverted);
}

static void format_count(uint32_t value, char *dest, size_t dest_len)
{
    static const char units[] = {'\0', 'K', 'M', 'G', 'T', 'P', 'E'};
    double scaled = (double)value;
    size_t unit = 0;

    while (scaled >= 1000.0 && unit < sizeof(units) - 1) {
        scaled /= 1000.0;
        ++unit;
    }

    if (unit == 0) {
        snprintf(dest, dest_len, "%lu", (unsigned long)value);
    } else if (scaled >= 100.0) {
        snprintf(dest, dest_len, "%.0f%c", scaled, units[unit]);
    } else if (scaled >= 10.0) {
        snprintf(dest, dest_len, "%.1f%c", scaled, units[unit]);
    } else {
        snprintf(dest, dest_len, "%.2f%c", scaled, units[unit]);
    }
}

static void format_hashrate(double ghs, char *dest, size_t dest_len)
{
    if (ghs <= 0.0) {
        dest[0] = '\0';
        return;
    }
    if (ghs >= 1000.0) {
        snprintf(dest, dest_len, "%.2fTh", ghs / 1000.0);
    } else if (ghs >= 1.0) {
        snprintf(dest, dest_len, "%.0fGh", ghs);
    } else if (ghs >= 0.001) {
        snprintf(dest, dest_len, "%.0fMh", ghs * 1000.0);
    } else {
        snprintf(dest, dest_len, "%.0fKh", ghs * 1000000.0);
    }
}

static bool format_temp_if_valid(float value, char *dest, size_t dest_len)
{
    if (value <= 0.0f) {
        dest[0] = '\0';
        return false;
    }
    snprintf(dest, dest_len, "%.0fC", value);
    return true;
}

static void format_pool_host(const stratum_minimal_stats_t *stats, char *dest,
                             size_t dest_len)
{
    if (dest_len == 0) {
        return;
    }
    if (stats == NULL || stats->pool_host[0] == '\0') {
        strlcpy(dest, "POOL --", dest_len);
        return;
    }
    strlcpy(dest, stats->pool_host, dest_len);
}

static void add_temp_line(int *row, float asic_temp_c, float vr_temp_c)
{
    char parts[2][16];
    char line[48];
    char asic_temp[8];
    char vr_temp[8];
    int count = 0;

    if (format_temp_if_valid(asic_temp_c, asic_temp, sizeof(asic_temp))) {
        snprintf(parts[count], sizeof(parts[count]), "ASIC: %s", asic_temp);
        count++;
    }

    if (format_temp_if_valid(vr_temp_c, vr_temp, sizeof(vr_temp))) {
        snprintf(parts[count], sizeof(parts[count]), "VR: %s", vr_temp);
        count++;
    }

    if (count == 0) {
        return;
    }

    if (count == 2) {
        snprintf(line, sizeof(line), "%s, %s", parts[0], parts[1]);
    } else {
        snprintf(line, sizeof(line), "%s", parts[0]);
    }
    oled_add_line(row, line);
}

static void draw_runtime_mining_qr_view(const stratum_minimal_stats_t *stats,
                                        float asic_temp_c, float vr_temp_c)
{
    char hashrate[12];
    char shares[32];
    char temps[24] = "TEMP --";
    char accepted[12];
    char rejected[12];
    char asic[8];
    char vr[8];
    char pool[64];

    format_hashrate(stats->measured_hashrate_ghs, hashrate, sizeof(hashrate));
    if (hashrate[0] == '\0') {
        strlcpy(hashrate, "MINING", sizeof(hashrate));
    }

    format_count(stats->accepted, accepted, sizeof(accepted));
    format_count(stats->rejected, rejected, sizeof(rejected));
    snprintf(shares, sizeof(shares), "A:%s R:%s", accepted, rejected);

    const bool have_asic_temp = format_temp_if_valid(asic_temp_c, asic, sizeof(asic));
    const bool have_vr_temp = format_temp_if_valid(vr_temp_c, vr, sizeof(vr));
    if (have_asic_temp && have_vr_temp) {
        snprintf(temps, sizeof(temps), "ASIC %s VR %s", asic, vr);
    } else if (have_asic_temp) {
        snprintf(temps, sizeof(temps), "ASIC %s", asic);
    } else if (have_vr_temp) {
        snprintf(temps, sizeof(temps), "VR %s", vr);
    }

    format_pool_host(stats, pool, sizeof(pool));
    oled_draw_runtime_qr_lines(hashrate, shares, temps, pool,
                               m45_config_get()->display_screensaver_enabled &&
                                   g_runtime_qr_on_right);
}

static void draw_status_view(GlobalState *state, const stratum_minimal_stats_t *stats,
                             float asic_temp_c, float vr_temp_c)
{
    int row = 0;

    if (!m45_config_hardware_identity_allowed()) {
        char board[32];
        snprintf(board, sizeof(board), "AXEOS BOARD %s",
                 m45_config_imported_board_version());
        oled_add_line(&row, "UNSUPPORTED BOARD");
        oled_add_line(&row, board);
        oled_add_line(&row, "ASIC DISABLED");
        oled_add_line(&row, wifi_http_retained_axeos_available()
                                ? "PRESS BOOT: AXEOS"
                                : "USB FLASH REQUIRED");
        return;
    }

    if (state->SYSTEM_MODULE.hardware_fault) {
        oled_add_line(&row, "SAFETY STOP");
        add_temp_line(&row, asic_temp_c, vr_temp_c);
        return;
    }

    if (!wifi_http_connected()) {
        if (wifi_http_setup_active()) {
            oled_draw_setup_qr_view();
            return;
        }
        oled_add_line(&row, "WIFI WAIT");
        add_temp_line(&row, asic_temp_c, vr_temp_c);
        return;
    }

    if (!state->ASIC_initalized) {
        oled_draw_runtime_qr_view("ASIC START");
        return;
    }

    if (!stats->connected) {
        oled_draw_runtime_qr_view("POOL WAIT");
        return;
    }

    draw_runtime_mining_qr_view(stats, asic_temp_c, vr_temp_c);
}

esp_err_t m45_oled_init(void)
{
    i2c_master_bus_handle_t bus = NULL;
    ESP_RETURN_ON_ERROR(i2c_bitaxe_get_master_bus_handle(&bus), TAG, "I2C bus unavailable");

    esp_lcd_panel_io_i2c_config_t io_config = {
        .dev_addr = CONFIG_M45_BITAXE_OLED_I2C_ADDR,
        .scl_speed_hz = 400000,
        .control_phase_bytes = 1,
        .dc_bit_offset = 6,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(bus, &io_config, &io), TAG,
                        "OLED I2C attach failed");

    esp_lcd_panel_ssd1306_config_t ssd1306_config = {
        .height = OLED_HEIGHT,
    };
    esp_lcd_panel_dev_config_t panel_config = {
        .bits_per_pixel = 1,
        .reset_gpio_num = -1,
        .vendor_config = &ssd1306_config,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_ssd1306(io, &panel_config, &g_panel), TAG,
                        "SSD1306 panel create failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(g_panel), TAG, "OLED reset failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(g_panel), TAG, "OLED init failed");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_disp_on_off(g_panel, true), TAG, "OLED on failed");
    gpio_config_t button_config = {
        .pin_bit_mask = 1ULL << CONFIG_GPIO_BUTTON_BOOT,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&button_config), TAG, "OLED alert button config failed");
    g_last_oled_input_tick = xTaskGetTickCount();
    g_button_was_pressed = oled_alert_button_pressed();
    ESP_LOGI(TAG, "OLED ready: %dx%d", OLED_WIDTH, OLED_HEIGHT);
    return ESP_OK;
}

static void oled_status_task(void *arg)
{
    GlobalState *state = (GlobalState *)arg;

    while (true) {
        stratum_minimal_stats_t stats;
        stratum_minimal_get_stats(&stats);
        const m45_config_t *config = m45_config_get();
        const TickType_t now = xTaskGetTickCount();
        const bool button_pressed = oled_alert_button_pressed();
        const bool button_edge = button_pressed && !g_button_was_pressed;
        g_button_was_pressed = button_pressed;

        if (button_edge && !m45_config_hardware_identity_allowed() &&
            wifi_http_retained_axeos_available()) {
            memset(g_frame, 0, sizeof(g_frame));
            oled_draw_centered_scaled(8, "RETURNING TO", 1);
            oled_draw_centered_scaled(18, "AXEOS", 1);
            g_full_refresh = true;
            oled_flush();
            const esp_err_t err = wifi_http_select_retained_axeos();
            if (err == ESP_OK) {
                vTaskDelay(pdMS_TO_TICKS(500));
                esp_restart();
            }
            ESP_LOGE(TAG, "failed to select retained AxeOS firmware: %s",
                     esp_err_to_name(err));
        }

        if (button_edge) {
            const bool woke_display = oled_record_input(now);
            if (stats.block_alert_active && !woke_display) {
                stratum_minimal_dismiss_block_alert();
                oled_set_invert(false);
                stats.block_alert_active = false;
            }
        }

        oled_update_sleep_state(now, stats.block_alert_active, config);
        if (g_oled_sleeping) {
            vTaskDelay(pdMS_TO_TICKS(OLED_STATUS_REFRESH_MS));
            continue;
        }

        memset(g_frame, 0, sizeof(g_frame));
        if (g_factory_reset_view) {
            oled_set_invert(false);
            oled_draw_factory_reset_view();
            oled_flush();
            vTaskDelay(pdMS_TO_TICKS(OLED_STATUS_REFRESH_MS));
            continue;
        }

        bitaxe_gamma602_power_snapshot_t power;
        const bool have_power = bitaxe_gamma602_power_snapshot(&power);
        const float asic_temp_c = state->POWER_MANAGEMENT_MODULE.chip_temp_avg;
        const float vr_temp_c = have_power ? (float)power.read_temp_c
                                           : state->POWER_MANAGEMENT_MODULE.vr_temp;
        const bool mining_active = oled_mining_view_active(state, &stats);

        if (stats.block_alert_active) {
            draw_block_found_alert();
            oled_set_invert(((xTaskGetTickCount() / pdMS_TO_TICKS(1000)) & 1U) != 0);
        } else {
            if (mining_active) {
                oled_update_screensaver_state(config, &stats);
            } else {
                oled_reset_runtime_screensaver(false);
            }
            draw_status_view(state, &stats, asic_temp_c, vr_temp_c);
        }

        oled_flush();
        const uint32_t delay_ms = stats.block_alert_active
                                      ? OLED_ALERT_REFRESH_MS
                                      : (mining_active ? OLED_MINING_REFRESH_MS
                                                       : OLED_STATUS_REFRESH_MS);
        vTaskDelay(pdMS_TO_TICKS(delay_ms));
    }
}

void m45_oled_start_status_task(GlobalState *state)
{
    xTaskCreate(oled_status_task, "oled_status", 6144, state, 2, NULL);
}

m45_oled_recovery_action_t m45_oled_recovery_action(void)
{
    if (g_panel == NULL || !oled_alert_button_pressed()) {
        return M45_OLED_RECOVERY_NONE;
    }

    const TickType_t started = xTaskGetTickCount();
    const TickType_t hold_ticks = pdMS_TO_TICKS(OLED_RECOVERY_HOLD_MS);
    while (oled_alert_button_pressed()) {
        const TickType_t elapsed = xTaskGetTickCount() - started;
        if (elapsed >= hold_ticks) {
            memset(g_frame, 0, sizeof(g_frame));
            oled_draw_centered_scaled(8, "RECOVERY MODE", 1);
            oled_draw_centered_scaled(18, "RESETTING SETTINGS", 1);
            g_full_refresh = true;
            oled_flush();
            return M45_OLED_RECOVERY_FACTORY_RESET;
        }
        const uint32_t remaining_ms = OLED_RECOVERY_HOLD_MS - pdTICKS_TO_MS(elapsed);
        memset(g_frame, 0, sizeof(g_frame));
        oled_draw_recovery_view((remaining_ms + 999U) / 1000U);
        g_full_refresh = true;
        oled_flush();
        vTaskDelay(pdMS_TO_TICKS(OLED_RECOVERY_POLL_MS));
    }

    return M45_OLED_RECOVERY_NONE;
}

void m45_oled_show_wifi_recovery(void)
{
    if (g_panel == NULL) {
        return;
    }
    memset(g_frame, 0, sizeof(g_frame));
    oled_draw_centered_scaled(8, "SETUP AP MODE", 1);
    oled_draw_centered_scaled(18, "SET NEW WIFI", 1);
    g_full_refresh = true;
    oled_flush();
}

void m45_oled_show_factory_reset(void)
{
    if (g_panel == NULL) {
        return;
    }
    g_factory_reset_view = true;
    oled_display_wake();
    oled_set_invert(false);
    memset(g_frame, 0, sizeof(g_frame));
    oled_draw_factory_reset_view();
    g_full_refresh = true;
    oled_flush();
}

#else

esp_err_t m45_oled_init(void)
{
    return ESP_OK;
}

void m45_oled_start_status_task(GlobalState *state)
{
    (void)state;
}

void m45_oled_show_factory_reset(void)
{
}

void m45_oled_show_wifi_recovery(void)
{
}

m45_oled_recovery_action_t m45_oled_recovery_action(void)
{
    return M45_OLED_RECOVERY_NONE;
}

#endif
