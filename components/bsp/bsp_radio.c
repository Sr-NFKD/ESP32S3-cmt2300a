#include "bsp_radio.h"
#include "bsp_board.h"
#include <string.h>
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bsp_radio";
static spi_device_handle_t radio;
static SemaphoreHandle_t lock;
static bool ready;

static esp_err_t wait_ready(void)
{
    int64_t deadline = esp_timer_get_time() + 10000;
    while (gpio_get_level(BSP_RADIO_MISO)) {
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        esp_rom_delay_us(10);
    }
    return ESP_OK;
}

static esp_err_t transfer(uint8_t header, const uint8_t *tx, uint8_t *rx,
                          size_t length, uint8_t *status)
{
    if (!radio || !lock) return ESP_ERR_INVALID_STATE;
    if (length > 64) return ESP_ERR_INVALID_SIZE;
    uint8_t out[65] = {header}, in[65] = {0};
    if (tx) memcpy(out + 1, tx, length);
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_err_t err = spi_device_acquire_bus(radio, portMAX_DELAY);
    if (err == ESP_OK) {
        gpio_set_level(BSP_RADIO_CS, 0);
        err = wait_ready();
        if (err == ESP_OK) {
            spi_transaction_t t = {
                .length = (length + 1) * 8, .tx_buffer = out, .rx_buffer = in,
            };
            err = spi_device_polling_transmit(radio, &t);
            if (err == ESP_OK && header == 0x30) err = wait_ready(); // SRES
        }
        gpio_set_level(BSP_RADIO_CS, 1);
        spi_device_release_bus(radio);
    }
    xSemaphoreGive(lock);
    if (err == ESP_OK) {
        if (status) *status = in[0];
        if (rx) memcpy(rx, in + 1, length);
    }
    return err;
}

esp_err_t bsp_radio_strobe(uint8_t command, uint8_t *status)
{
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (command < 0x30 || command > 0x3D || command == 0x37)
        return ESP_ERR_INVALID_ARG;
    return transfer(command, NULL, NULL, 0, status);
}

esp_err_t bsp_radio_read_reg(uint8_t address, uint8_t *value)
{
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (!value || address > 0x3F || address == 0x2F) return ESP_ERR_INVALID_ARG;
    uint8_t header = address | 0x80;
    if (address >= 0x30 && address <= 0x3D) header |= 0x40;
    return transfer(header, NULL, value, 1, NULL);
}

esp_err_t bsp_radio_write_reg(uint8_t address, uint8_t value)
{
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (address > 0x3F || (address >= 0x2F && address <= 0x3D))
        return ESP_ERR_INVALID_ARG;
    return transfer(address, &value, NULL, 1, NULL);
}

static bool valid_burst(uint8_t address, size_t length)
{
    return length > 0 && length <= 64 &&
        ((address <= 0x2E && length <= (size_t)(0x2F - address)) ||
         (address == 0x3E && length <= 8) || address == 0x3F);
}

esp_err_t bsp_radio_read_burst(uint8_t address, uint8_t *data, size_t length)
{
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (!data || !valid_burst(address, length)) return ESP_ERR_INVALID_ARG;
    return transfer(address | 0xC0, NULL, data, length, NULL);
}

esp_err_t bsp_radio_write_burst(uint8_t address, const uint8_t *data, size_t length)
{
    if (!ready) return ESP_ERR_INVALID_STATE;
    if (!data || !valid_burst(address, length)) return ESP_ERR_INVALID_ARG;
    return transfer(address | 0x40, data, NULL, length, NULL);
}

esp_err_t bsp_radio_init(bsp_radio_info_t *info)
{
    if (!info) return ESP_ERR_INVALID_ARG;
    if (radio) return ESP_ERR_INVALID_STATE;
    const gpio_config_t outputs = {
        .pin_bit_mask = (1ULL << BSP_RADIO_CS) | (1ULL << BSP_RADIO_SCLK) |
                        (1ULL << BSP_RADIO_MOSI), .mode = GPIO_MODE_OUTPUT,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&outputs), TAG, "reset pins");
    gpio_set_level(BSP_RADIO_CS, 1);
    gpio_set_level(BSP_RADIO_SCLK, 1);
    gpio_set_level(BSP_RADIO_MOSI, 0);
    gpio_set_level(BSP_RADIO_CS, 0);
    esp_rom_delay_us(10);
    gpio_set_level(BSP_RADIO_CS, 1);
    esp_rom_delay_us(50);
    const gpio_config_t inputs = {
        .pin_bit_mask = (1ULL << BSP_RADIO_GDO0) | (1ULL << BSP_RADIO_GDO2) |
                        (1ULL << BSP_RADIO_MISO), .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&inputs), TAG, "radio inputs");
    const spi_bus_config_t bus = {
        .mosi_io_num = BSP_RADIO_MOSI, .miso_io_num = BSP_RADIO_MISO,
        .sclk_io_num = BSP_RADIO_SCLK, .quadwp_io_num = -1, .quadhd_io_num = -1,
        .max_transfer_sz = 65,
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(BSP_RADIO_SPI_HOST, &bus, SPI_DMA_DISABLED), TAG, "SPI bus");
    const spi_device_interface_config_t device = {
        .clock_speed_hz = BSP_RADIO_SPI_HZ, .mode = 0,
        .spics_io_num = -1, .queue_size = 1,
    };
    esp_err_t err = spi_bus_add_device(BSP_RADIO_SPI_HOST, &device, &radio);
    if (err != ESP_OK) {
        spi_bus_free(BSP_RADIO_SPI_HOST);
        return err;
    }
    lock = xSemaphoreCreateMutex();
    if (!lock) { err = ESP_ERR_NO_MEM; goto fail; }
    err = transfer(0x30, NULL, NULL, 0, NULL);
    if (err != ESP_OK) goto fail;
    ready = true;
    err = bsp_radio_read_reg(0x30, &info->partnum);
    if (err == ESP_OK) err = bsp_radio_read_reg(0x31, &info->version);
    if (err == ESP_OK && (info->partnum != 0 || info->version == 0 || info->version == 0xFF))
        err = ESP_ERR_NOT_FOUND;
    if (err == ESP_OK) return ESP_OK;
fail:
    ready = false;
    if (lock) { vSemaphoreDelete(lock); lock = NULL; }
    spi_bus_remove_device(radio);
    radio = NULL;
    spi_bus_free(BSP_RADIO_SPI_HOST);
    return err;
}
