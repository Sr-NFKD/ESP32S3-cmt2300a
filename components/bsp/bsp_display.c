#include "bsp_display.h"
#include "bsp_buttons.h"
#include "bsp_backlight.h"

#include "bsp_board.h"
#include <stdlib.h>
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_display";
static esp_lcd_panel_io_handle_t io;
static lv_display_t *display;
static lv_indev_t *keypad;
static SemaphoreHandle_t color_done;
static void *buffers[2];
// Only the LVGL owner task changes this; the ISR signals completion via semaphore.
static bool transfer_pending;

// Parameters from the supplied HSD0.96IPS/ST7735S Gamma2.2 sequence.
typedef struct { uint8_t cmd, length, data[16]; } lcd_init_cmd_t;
static const lcd_init_cmd_t init_commands[] = {
    {0xB1, 3, {0x05, 0x3C, 0x3C}},
    {0xB2, 3, {0x05, 0x3C, 0x3C}},
    {0xB3, 6, {0x05, 0x3C, 0x3C, 0x05, 0x3C, 0x3C}},
    {0xB4, 1, {0x03}}, {0xC0, 3, {0x04, 0x04, 0x84}},
    {0xC1, 1, {0x06}}, {0xC2, 2, {0x0D, 0x00}},
    {0xC3, 2, {0x8D, 0x6A}}, {0xC4, 2, {0x8D, 0xEE}},
    {0xC5, 1, {0x3A}},
    {0xE0, 16, {0x10, 0x0E, 0x05, 0x0D, 0x0E, 0x06, 0x03, 0x07,
                0x0A, 0x0E, 0x1D, 0x32, 0x03, 0x0B, 0x00, 0x2F}},
    {0xE1, 16, {0x0E, 0x12, 0x05, 0x07, 0x0C, 0x05, 0x02, 0x06,
                0x07, 0x0B, 0x13, 0x2C, 0x03, 0x0D, 0x02, 0x1F}},
    {0x3A, 1, {0x05}}, // RGB565
    {0x36, 1, {BSP_LCD_MADCTL}},
    {0x21, 0, {0}}, // IPS inversion on
    {0x29, 0, {0}},
};

static bool on_color_done(esp_lcd_panel_io_handle_t panel_io,
                          esp_lcd_panel_io_event_data_t *event, void *ctx)
{
    (void)panel_io;
    (void)event;
    BaseType_t wake = pdFALSE;
    xSemaphoreGiveFromISR((SemaphoreHandle_t)ctx, &wake);
    return wake == pdTRUE;
}

static uint32_t tick_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

static void wait_flush(lv_display_t *disp)
{
    (void)disp;
    if (!transfer_pending) return;
    if (xSemaphoreTake(color_done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE(TAG, "LCD DMA timeout; stop rather than reuse an active buffer");
        abort();
    }
    transfer_pending = false;
    // LVGL clears its flushing flag after this callback returns.
}

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *pixels)
{
    uint16_t x1 = area->x1 + BSP_LCD_X_OFFSET, x2 = area->x2 + BSP_LCD_X_OFFSET;
    uint16_t y1 = area->y1 + BSP_LCD_Y_OFFSET, y2 = area->y2 + BSP_LCD_Y_OFFSET;
    const uint8_t col[] = {x1 >> 8, x1 & 0xFF, x2 >> 8, x2 & 0xFF};
    const uint8_t row[] = {y1 >> 8, y1 & 0xFF, y2 >> 8, y2 & 0xFF};
    uint32_t count = (area->x2 - area->x1 + 1) * (area->y2 - area->y1 + 1);
    lv_draw_sw_rgb565_swap(pixels, count);
    esp_err_t err = esp_lcd_panel_io_tx_param(io, 0x2A, col, sizeof(col));
    if (err == ESP_OK) err = esp_lcd_panel_io_tx_param(io, 0x2B, row, sizeof(row));
    if (err == ESP_OK) {
        transfer_pending = true;
        err = esp_lcd_panel_io_tx_color(io, 0x2C, pixels, count * 2);
    }
    if (err != ESP_OK) {
        transfer_pending = false;
        ESP_LOGE(TAG, "LCD flush failed: %s", esp_err_to_name(err));
        lv_display_flush_ready(disp);
    }
    // Return immediately: render into the other buffer while DMA sends this one.
    // wait_flush() consumes the completion before the next transfer/buffer reuse.
}

static void read_keypad(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    static uint32_t last_key = LV_KEY_ENTER;
    const uint32_t codes[BSP_KEY_COUNT] = {
        LV_KEY_UP, LV_KEY_RIGHT, LV_KEY_LEFT, LV_KEY_ENTER, LV_KEY_DOWN
    };
    uint32_t mask = bsp_buttons_pressed();
    data->state = LV_INDEV_STATE_RELEASED;
    for (int i = 0; i < BSP_KEY_COUNT; i++) {
        if (mask & (1U << i)) {
            last_key = codes[i];
            data->state = LV_INDEV_STATE_PRESSED;
            break;
        }
    }
    data->key = last_key;
}

static esp_err_t panel_start(void)
{
    esp_err_t err = gpio_set_level(BSP_LCD_RST, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(10));
    err = gpio_set_level(BSP_LCD_RST, 1);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    err = esp_lcd_panel_io_tx_param(io, 0x11, NULL, 0);
    if (err != ESP_OK) return err;
    vTaskDelay(pdMS_TO_TICKS(120));
    for (size_t i = 0; i < sizeof(init_commands) / sizeof(init_commands[0]); i++) {
        err = esp_lcd_panel_io_tx_param(io, init_commands[i].cmd,
                                       init_commands[i].data, init_commands[i].length);
        if (err != ESP_OK) return err;
    }
    return ESP_OK;
}

esp_err_t bsp_display_init(void)
{
    if (display || io) return ESP_ERR_INVALID_STATE;
    // Also verifies bsp_init() has configured the PWM channel.
    ESP_RETURN_ON_ERROR(bsp_backlight_set(0), TAG, "backlight off");
    const size_t bytes = BSP_LCD_WIDTH * BSP_LCD_BUFFER_LINES * 2;
    color_done = xSemaphoreCreateBinary();
    buffers[0] = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    buffers[1] = heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    transfer_pending = false;
    if (!color_done || !buffers[0] || !buffers[1]) {
        if (color_done) vSemaphoreDelete(color_done);
        color_done = NULL;
        for (unsigned i = 0; i < 2; i++) { heap_caps_free(buffers[i]); buffers[i] = NULL; }
        return ESP_ERR_NO_MEM;
    }
    const spi_bus_config_t bus = {
        .mosi_io_num = BSP_LCD_MOSI, .miso_io_num = -1,
        .sclk_io_num = BSP_LCD_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = bytes,
    };
    esp_err_t err = spi_bus_initialize(BSP_LCD_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    bool bus_initialized = err == ESP_OK;
    if (err != ESP_OK) goto fail;
    const esp_lcd_panel_io_spi_config_t config = {
        .cs_gpio_num = BSP_LCD_CS, .dc_gpio_num = BSP_LCD_DC, .spi_mode = 0,
        .pclk_hz = BSP_LCD_SPI_HZ, .trans_queue_depth = 1,
        .lcd_cmd_bits = 8, .lcd_param_bits = 8,
        .on_color_trans_done = on_color_done, .user_ctx = color_done,
    };
    err = esp_lcd_new_panel_io_spi(BSP_LCD_SPI_HOST, &config, &io);
    if (err != ESP_OK) goto fail;
    err = panel_start();
    if (err != ESP_OK) goto fail;
    lv_init();
    lv_tick_set_cb(tick_ms);
    display = lv_display_create(BSP_LCD_WIDTH, BSP_LCD_HEIGHT);
    if (!display) { err = ESP_ERR_NO_MEM; goto fail; }
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, buffers[0], buffers[1], bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_display_set_flush_wait_cb(display, wait_flush);
    keypad = lv_indev_create();
    if (!keypad) { err = ESP_ERR_NO_MEM; goto fail; }
    lv_indev_set_type(keypad, LV_INDEV_TYPE_KEYPAD);
    lv_indev_set_display(keypad, display);
    lv_indev_set_read_cb(keypad, read_keypad);
    return ESP_OK; // Application draws its first frame before enabling backlight.
fail:
    if (keypad) { lv_indev_delete(keypad); keypad = NULL; }
    if (display) { lv_display_delete(display); display = NULL; }
    if (io) { esp_lcd_panel_io_del(io); io = NULL; }
    if (bus_initialized) spi_bus_free(BSP_LCD_SPI_HOST);
    vSemaphoreDelete(color_done);
    color_done = NULL;
    for (unsigned i = 0; i < 2; i++) { heap_caps_free(buffers[i]); buffers[i] = NULL; }
    return err;
}

esp_err_t bsp_display_sleep(bool sleeping)
{
    wait_flush(display);
    // Reuse cold-start reset/configuration: sleep-out alone does not restore
    // a panel that lost its display-enable, pixel-format or scan settings.
    if (!sleeping) return panel_start();
    esp_err_t err = esp_lcd_panel_io_tx_param(io, 0x28, NULL, 0); // DISPOFF
    if (err == ESP_OK) err = esp_lcd_panel_io_tx_param(io, 0x10, NULL, 0); // SLPIN
    // Finish the panel power-down sequence before sleeping the MCU.
    if (err == ESP_OK) vTaskDelay(pdMS_TO_TICKS(120));
    return err;
}

lv_display_t *bsp_display_get(void) { return display; }
lv_indev_t *bsp_keypad_get(void) { return keypad; }
