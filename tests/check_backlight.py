"""Check the actual active-low backlight mapping without hardware."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
source = (root / "components/bsp/bsp_backlight.c").read_text(encoding="utf-8")
assert ".flags.output_invert = 1" in source
assert 'gpio_set_level(BSP_LCD_LED, 1), TAG, "backlight off"' in (root / "components/bsp/bsp.c").read_text(encoding="utf-8")
function = source[source.index("esp_err_t bsp_backlight_set("):source.index("esp_err_t bsp_backlight_init(")]
prefix = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_INVALID_ARG 2
#define LEDC_LOW_SPEED_MODE 0
#define LEDC_CHANNEL_0 0
#define ESP_RETURN_ON_ERROR(expr,...) do { int err=(expr); if(err) return err; } while(0)
static bool initialized;
static uint32_t duty;
static unsigned writes, updates;
static int set_error, update_error;
static int ledc_set_duty(int mode,int channel,uint32_t value) {
    (void)mode;(void)channel;writes++;duty=value;return set_error;
}
static int ledc_update_duty(int mode,int channel) {
    (void)mode;(void)channel;updates++;return update_error;
}
'''
checks = r'''
int main(void) {
    assert(bsp_backlight_set(50)==ESP_ERR_INVALID_STATE && writes==0);
    initialized=true;
    assert(bsp_backlight_set(101)==ESP_ERR_INVALID_ARG && writes==0);
    assert(bsp_backlight_set(0)==ESP_OK && duty==0);
    assert(bsp_backlight_set(1)==ESP_OK && duty==0); // physical pin always HIGH
    uint32_t previous=0;
    for(unsigned level=2;level<=100;level++) {
        assert(bsp_backlight_set(level)==ESP_OK);
        assert(duty>previous && duty<=1024);
        // Within half a tick of the linear mapping across all 100 levels.
        int error=(int)(duty*99)-(int)(1024*(level-1));
        assert(error>=-49 && error<=49);
        previous=duty;
    }
    assert(duty==1024); // physical pin HIGH for exactly half of 2048 ticks
    unsigned before=updates;
    set_error=9;assert(bsp_backlight_set(50)==9 && updates==before);
    set_error=0;update_error=8;assert(bsp_backlight_set(50)==8);
    puts("PASS: all 100 levels, 100%-50% high-duty endpoints, polarity, off and error propagation");
}
'''
compiler = root / "simulator/.tools/w64devkit/bin/gcc.exe"
env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ["PATH"])
with tempfile.TemporaryDirectory(prefix="rf-backlight-") as directory:
    test = Path(directory) / "test.c"
    test.write_text(prefix + function + checks, encoding="utf-8")
    binary = Path(directory) / "check.exe"
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)
