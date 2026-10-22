"""Run the real idle/sleep controller with deterministic time and hardware stubs.

Does not measure board current or prove electrical power-off/wakeup behavior.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/app/app_power.c").read_text(encoding="utf-8")
controller = source[source.index("static void sleep_if_idle("):source.index("void app_power_sync_backlight(")]
prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <setjmp.h>
typedef int esp_err_t;
typedef int esp_timer_handle_t;
#define ESP_OK 0
#define DEBUG_OTA_OFF 0
#define ESP_SLEEP_WAKEUP_TIMER 1
#define ESP_ERROR_CHECK(e) assert((e)==0)
#define ESP_LOGW(...) ((void)0)
#define ESP_LOGI(...) ((void)0)
static uint8_t pending_light_minutes=10, pending_deep_minutes=20;
static uint8_t saved_light_minutes=10, saved_deep_minutes=20, applied_brightness=70;
static int64_t last_activity, now;
typedef struct { int64_t last_signal_us; } rf_status_t;
static int64_t last_signal_us;
static void rf_service_status(rf_status_t *s){s->last_signal_us=last_signal_us;}
static bool suppress_wake_key;
static bool usb_connected;
static bool usb_serial_jtag_is_connected(void){return usb_connected;}
static unsigned raw, debug, light_calls, deep_calls, wake_cause=2;
static int rf_error, display_error, light_error;
static bool rf_asleep, display_asleep, suspended, timer_stopped;
static uint8_t backlight=70;
static uint64_t delay;
static jmp_buf reboot;
static int64_t esp_timer_get_time(void){return now;}
static unsigned bsp_buttons_raw(void){return raw;}
static unsigned debug_ota_state(void){return debug;}
static int rf_service_sleep(bool sleep){if(sleep && rf_error)return rf_error;rf_asleep=sleep;return 0;}
static int esp_timer_stop(int t){(void)t;timer_stopped=true;return 0;}
static int esp_timer_start_periodic(int t,int us){(void)t;assert(us==1000);timer_stopped=false;return 0;}
static void bsp_buttons_suspend(void){suspended=true;}
static int bsp_backlight_set(uint8_t value){backlight=value;return 0;}
static int bsp_display_sleep(bool sleep){display_asleep=sleep;return sleep?display_error:0;}
static int bsp_light_sleep(uint64_t us){
    assert(rf_asleep && display_asleep && suspended && timer_stopped && !backlight);
    light_calls++;delay=us;now+=us;return light_error;
}
static unsigned esp_sleep_get_wakeup_cause(void){return wake_cause;}
static void bsp_deep_sleep(void){assert(rf_asleep && display_asleep && !backlight);deep_calls++;longjmp(reboot,1);}
static void rf_ui_radio_error(const char *s){(void)s;}
static const char *esp_err_to_name(int e){(void)e;return "error";}
static void rf_ui_set_keys(unsigned keys){assert(!keys);}
static void *lv_screen_active(void){return NULL;}
static void lv_obj_invalidate(void *o){(void)o;}
static void *bsp_display_get(void){return NULL;}
static void lv_refr_now(void *d){(void)d;}
'''
checks = r'''
int main(void){
    now=600000000-1; sleep_if_idle(1); assert(!light_calls);
    now++; sleep_if_idle(1);
    assert(light_calls==1 && delay==1200000000ULL && !deep_calls);
    assert(!rf_asleep && !display_asleep && !timer_stopped && backlight==70 && suppress_wake_key);
    assert(last_activity==now); // Button wake restarts the full idle interval.
    now+=599999999; sleep_if_idle(1); assert(light_calls==1);
    now++; wake_cause=ESP_SLEEP_WAKEUP_TIMER;
    if(!setjmp(reboot)) {sleep_if_idle(1);assert(0);}
    assert(deep_calls==1 && light_calls==2 && delay==1200000000ULL);
    // Disabled first stage never sleeps, even after a day.
    pending_light_minutes=saved_light_minutes=0;now+=86400000000LL;
    sleep_if_idle(1);assert(light_calls==2);
    // Disabled second stage has no timer and cannot fall through to deep sleep.
    pending_light_minutes=saved_light_minutes=1;pending_deep_minutes=saved_deep_minutes=0;
    sleep_if_idle(1);assert(light_calls==3 && !delay && deep_calls==1);
    raw=1;now+=60000000; sleep_if_idle(1);assert(light_calls==3 && last_activity==now);
    raw=0;debug=1;now+=60000000;sleep_if_idle(1);assert(light_calls==3 && last_activity==now);
    debug=0;now+=60000000;saved_light_minutes=2;sleep_if_idle(1);assert(light_calls==3);
    saved_light_minutes=1;rf_error=1;sleep_if_idle(1);assert(light_calls==3 && last_activity==now);
    rf_error=0;display_error=1;now+=60000000;sleep_if_idle(1);
    assert(light_calls==3 && !rf_asleep && !display_asleep && !timer_stopped && backlight==70);
    display_error=0;light_error=1;pending_deep_minutes=saved_deep_minutes=20;now+=60000000;sleep_if_idle(1);
    assert(light_calls==4 && deep_calls==1 && !rf_asleep && !display_asleep && backlight==70);
    // RF activity keeps the device awake across multiple idle periods.
    light_error=0;pending_deep_minutes=saved_deep_minutes=0;
    for(unsigned i=0;i<180;i++) {
        now+=1000000;last_signal_us=now;sleep_if_idle(1);assert(light_calls==4);
    }
    // After reception stops, wait the full configured idle period.
    now=last_signal_us+59999999;sleep_if_idle(1);assert(light_calls==4);
    now++;sleep_if_idle(1);assert(light_calls==5);
    // An old RF timestamp must not override newer button activity.
    now+=30000000;last_activity=now;
    now+=59999999;sleep_if_idle(1);assert(light_calls==5);
    now++;sleep_if_idle(1);assert(light_calls==6);
    usb_connected=true;now+=60000000;sleep_if_idle(1);
    assert(light_calls==6 && last_activity==now);
    now+=600000000;sleep_if_idle(1);assert(light_calls==6 && last_activity==now);
    usb_connected=false;now+=59999999;sleep_if_idle(1);assert(light_calls==6);
    now++;sleep_if_idle(1);assert(light_calls==7);
    puts("PASS: sleep policy, USB keep-awake/disconnect countdown, RF activity, OTA guard and failure recovery");
}
'''
compiler = root / "simulator/.tools/w64devkit/bin/gcc.exe"
env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ["PATH"])
with tempfile.TemporaryDirectory(prefix="rf-sleep-") as directory:
    test = Path(directory) / "test.c"
    test.write_text(prefix + controller + checks, encoding="utf-8")
    binary = Path(directory) / "check.exe"
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)
