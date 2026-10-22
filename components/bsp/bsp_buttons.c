#include "bsp_buttons.h"

#include "bsp_board.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"

#include "bsp_private.h"

static const char *TAG = "bsp_buttons";
static const gpio_num_t keys[BSP_KEY_COUNT] = {
    BSP_BUTTON_UP, BSP_BUTTON_RIGHT, BSP_BUTTON_LEFT, BSP_BUTTON_PRESS, BSP_BUTTON_DOWN
};
static bool initialized;
static uint32_t pressed, stable_keys;
static TaskHandle_t button_task_handle, button_owner;
static QueueHandle_t button_events;
static bool candidate[BSP_KEY_COUNT];
static int64_t changed_at[BSP_KEY_COUNT];

esp_err_t bsp_buttons_init(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < BSP_KEY_COUNT; i++) mask |= 1ULL << keys[i];
    const gpio_config_t inputs = {
        .pin_bit_mask = mask, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&inputs), TAG, "button pullups");
    initialized = true;
    return ESP_OK;
}

// Only the button task owns candidate, changed_at and stable_keys.
static bool buttons_sample(uint32_t edges, int64_t now)
{
    bool pending = false;
    for (int i = 0; i < BSP_KEY_COUNT; i++) {
        bool down = gpio_get_level(keys[i]) == 0;
        if ((edges & (1U << i)) || down != candidate[i]) {
            candidate[i] = down;
            changed_at[i] = now;
        }
        if (now - changed_at[i] >= BSP_BUTTON_DEBOUNCE_MS * 1000LL) {
            if (down) stable_keys |= 1U << i;
            else stable_keys &= ~(1U << i);
        } else pending = true;
    }
    return pending;
}

static void button_isr(void *arg)
{
    BaseType_t wake = pdFALSE;
    xTaskNotifyFromISR(button_task_handle, 1U << (uintptr_t)arg, eSetBits, &wake);
    if (wake) portYIELD_FROM_ISR();
}

static void buttons_task(void *arg)
{
    (void)arg;
    uint32_t edges = (1U << BSP_KEY_COUNT) - 1;
    for (;;) {
        uint32_t before = stable_keys;
        bool pending = buttons_sample(edges, esp_timer_get_time());
        if (stable_keys != before) {
            // Bounded backlog: on overflow discard oldest, retain the newest state
            // (including releases) so the UI cannot remain stuck in a held key.
            if (xQueueSend(button_events, &stable_keys, 0) != pdTRUE) {
                uint32_t discarded;
                xQueueReceive(button_events, &discarded, 0);
                xQueueSend(button_events, &stable_keys, 0);
                ESP_LOGW(TAG, "Button queue full; oldest event discarded");
            }
            xTaskNotifyGive(button_owner);
        }
        edges = 0;
        xTaskNotifyWait(0, UINT32_MAX, &edges, pending ? pdMS_TO_TICKS(1) : portMAX_DELAY);
    }
}

esp_err_t bsp_buttons_start(TaskHandle_t owner)
{
    if (!initialized || !owner || button_task_handle) return ESP_ERR_INVALID_STATE;
    button_events = xQueueCreate(32, sizeof(uint32_t));
    if (!button_events) return ESP_ERR_NO_MEM;
    button_owner = owner;
    esp_err_t err = gpio_install_isr_service(0);
    bool installed = err == ESP_OK;
    unsigned added = 0;
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) goto fail;
    if (xTaskCreate(buttons_task, "buttons", 3072, NULL, 5, &button_task_handle) != pdPASS) {
        err = ESP_ERR_NO_MEM; goto fail;
    }
    for (unsigned i = 0; i < BSP_KEY_COUNT; i++) {
        err = gpio_isr_handler_add(keys[i], button_isr, (void *)(uintptr_t)i);
        if (err != ESP_OK) goto fail;
        added++;
        err = gpio_set_intr_type(keys[i], GPIO_INTR_ANYEDGE);
        if (err == ESP_OK) err = gpio_intr_enable(keys[i]);
        if (err != ESP_OK) goto fail;
    }
    // Resample after enabling every interrupt, including keys held at startup.
    xTaskNotify(button_task_handle, (1U << BSP_KEY_COUNT) - 1, eSetBits);
    return ESP_OK;
fail:
    for (unsigned i = 0; i < added; i++) {
        gpio_intr_disable(keys[i]);
        gpio_isr_handler_remove(keys[i]);
    }
    if (button_task_handle) { vTaskDelete(button_task_handle); button_task_handle = NULL; }
    if (installed) gpio_uninstall_isr_service();
    vQueueDelete(button_events); button_events = NULL;
    return err;
}

bool bsp_buttons_take_event(uint32_t *mask)
{
    if (!mask || !button_events || xQueueReceive(button_events, mask, 0) != pdTRUE) return false;
    pressed = *mask; // UI-owned state also used by the LVGL keypad adapter.
    return true;
}

uint32_t bsp_buttons_pressed(void) { return pressed; }

uint32_t bsp_buttons_raw(void)
{
    uint32_t mask = 0;
    for (unsigned i = 0; i < BSP_KEY_COUNT; i++)
        if (!gpio_get_level(keys[i])) mask |= 1U << i;
    return mask;
}

void bsp_buttons_suspend(void)
{
    for (unsigned i = 0; i < BSP_KEY_COUNT; i++) gpio_intr_disable(keys[i]);
    vTaskSuspend(button_task_handle);
    xQueueReset(button_events);
    pressed = 0;
}

void bsp_buttons_resume(void)
{
    // Called only after the wake key has been released; discard its queued edges.
    xQueueReset(button_events);
    pressed = stable_keys = 0;
    for (unsigned i = 0; i < BSP_KEY_COUNT; i++) {
        candidate[i] = false; changed_at[i] = esp_timer_get_time();
        gpio_intr_enable(keys[i]);
    }
    vTaskResume(button_task_handle);
    xTaskNotify(button_task_handle, (1U << BSP_KEY_COUNT) - 1, eSetBits);
}
