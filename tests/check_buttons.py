"""Run the real button debounce/task/queue code with deterministic GPIO/RTOS stubs."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/bsp/bsp_buttons.c").read_text(encoding="utf-8")
assert "#define BSP_BUTTON_DEBOUNCE_MS 2\n" in (root / "components/bsp/include/bsp_board.h").read_text(encoding="utf-8")
callbacks = source[source.index("static bool buttons_sample("):source.index("esp_err_t bsp_buttons_start(")]
consumer = source[source.index("bool bsp_buttons_take_event("):]
prefix = r'''
#include <stdbool.h>
#include <stdint.h>
#include <assert.h>
#include <stdio.h>
#include <setjmp.h>
#define BSP_KEY_COUNT 5
#define BSP_BUTTON_DEBOUNCE_MS 2
#define pdTRUE 1
#define pdFALSE 0
#define eSetBits 1
#define portMAX_DELAY UINT32_MAX
#define pdMS_TO_TICKS(n) (n)
#define ESP_LOGW(...) (warnings++)
#define portYIELD_FROM_ISR() (yields++)
typedef int BaseType_t;
static const int keys[5] = {0,1,2,3,4};
static bool candidate[5];
static int64_t changed_at[5], now;
static uint32_t pressed, stable_keys, raw, notified;
static void *button_task_handle = (void *)1, *button_owner = (void *)2, *button_events = (void *)3;
static uint32_t queue[32];
static unsigned count, notices, warnings, yields, waits;
static jmp_buf done;
static int gpio_get_level(int pin) { return !(raw & (1U << pin)); }
static int64_t esp_timer_get_time(void) { return now; }
static unsigned suspended, enabled = 31;
static void gpio_intr_disable(int pin) { enabled &= ~(1U << pin); }
static void gpio_intr_enable(int pin) { enabled |= 1U << pin; }
static void vTaskSuspend(void *task) { assert(task == button_task_handle); suspended = 1; }
static void vTaskResume(void *task) { assert(task == button_task_handle); suspended = 0; }
static void xQueueReset(void *q) { assert(q == button_events); count = 0; }
static void xTaskNotify(void *task, uint32_t bits, int mode) {
    assert(task == button_task_handle && mode == eSetBits); notified |= bits;
}
static int xQueueSend(void *q, const uint32_t *value, unsigned timeout) {
    (void)timeout; assert(q == button_events);
    if (count == 32) return 0;
    queue[count++] = *value; return 1;
}
static int xQueueReceive(void *q, uint32_t *value, unsigned timeout) {
    (void)timeout; assert(q == button_events);
    if (!count) return 0;
    *value = queue[0]; count--;
    for (unsigned i = 0; i < count; i++) queue[i] = queue[i+1];
    return 1;
}
static void xTaskNotifyGive(void *task) { assert(task == button_owner); notices++; }
static void xTaskNotifyFromISR(void *task, uint32_t bits, int mode, int *wake) {
    assert(task == button_task_handle && mode == eSetBits); notified |= bits; *wake = 1;
}
static int xTaskNotifyWait(uint32_t clear_entry, uint32_t clear_exit, uint32_t *bits, uint32_t timeout) {
    assert(clear_entry == 0 && clear_exit == UINT32_MAX);
    waits++;
    if (waits == 1) { assert(timeout == 1); now += 20000; *bits = 0; return 0; }
    if (waits == 2) { assert(timeout == portMAX_DELAY); raw = 0; *bits = 1; now += 1000; return 1; }
    if (waits == 3) { assert(timeout == 1); now += 20000; *bits = 0; return 0; }
    assert(timeout == portMAX_DELAY); longjmp(done, 1);
}
'''
checks = r'''
int main(void) {
    // Bounce resets the fixed 2 ms window, including same-level IRQs.
    raw = 1; assert(buttons_sample(1, 0) && stable_keys == 0);
    raw = 0; assert(buttons_sample(1, 500) && stable_keys == 0);
    raw = 1; assert(buttons_sample(1, 1000));
    assert(buttons_sample(0, 2999) && stable_keys == 0);
    assert(!buttons_sample(0, 3000) && stable_keys == 1);
    assert(!buttons_sample(0, 900000) && stable_keys == 1);
    assert(buttons_sample(1, 910000));
    raw = 0; assert(buttons_sample(1, 911000) && stable_keys == 1);
    assert(buttons_sample(0, 912999) && stable_keys == 1);
    assert(!buttons_sample(0, 913000) && stable_keys == 0);
    raw = 17; assert(buttons_sample(17, 950000));
    assert(!buttons_sample(0, 952000) && stable_keys == 17);
    raw = 0; buttons_sample(17, 980000); buttons_sample(0, 982000);
    assert(stable_keys == 0);
    // ISR only notifies; no UI state mutation or queued action.
    button_isr((void *)(uintptr_t)3);
    assert(notified == 8 && yields == 1 && count == 0 && pressed == 0);
    // Startup-held key, idle sleep, ordered press/release while UI is blocked.
    raw = 1; now = 2000000;
    if (!setjmp(done)) buttons_task(NULL);
    assert(count == 2 && notices == 2 && queue[0] == 1 && queue[1] == 0);
    uint32_t value;
    assert(bsp_buttons_take_event(&value) && value == 1 && bsp_buttons_pressed() == 1);
    assert(bsp_buttons_take_event(&value) && value == 0 && bsp_buttons_pressed() == 0);
    assert(!bsp_buttons_take_event(&value) && !bsp_buttons_take_event(NULL));
    // Full queue must preserve the latest release, never leave a stuck key.
    for (unsigned i=0; i<32; i++) queue[count++] = 1;
    waits = 0; raw = 1; now += 100000;
    if (!setjmp(done)) buttons_task(NULL);
    assert(count == 32 && warnings == 2 && queue[30] == 1 && queue[31] == 0);
    raw = 17; assert(bsp_buttons_raw() == 17);
    bsp_buttons_suspend(); assert(suspended && !enabled && !count && !pressed);
    raw = 0; bsp_buttons_resume();
    assert(!suspended && enabled == 31 && !stable_keys && !count && !pressed);
    for (unsigned i = 0; i < 5; i++) assert(!candidate[i]);
    puts("PASS: bounce, hold, simultaneous keys, ISR, ordered events, sleep/resume and overflow release");
}
'''
compiler = root / "simulator/.tools/w64devkit/bin/gcc.exe"
env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ["PATH"])
with tempfile.TemporaryDirectory(prefix="rf-buttons-") as directory:
    test = Path(directory) / "test.c"
    test.write_text(prefix + callbacks + consumer + checks, encoding="utf-8")
    binary = Path(directory) / "check.exe"
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)
