"""Exercise the actual battery read function with a deterministic ADC curve."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
board = (root / 'components/bsp/include/bsp_board.h').read_text()
implementation = (root / 'components/bsp/bsp_battery.c').read_text()
source = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
enum { ESP_OK, ESP_ERR_INVALID_ARG, ESP_ERR_INVALID_STATE, ESP_ERR_INVALID_RESPONSE };
static int adc=1, calibration=1, channel, sample=2752, fail;
static int adc_oneshot_read(int a,int c,int *raw) {
    (void)a;(void)c;*raw=sample;return fail;
}
static int adc_cali_raw_to_voltage(int c,int raw,int *mv) {
    (void)c;*mv=raw/3+10;return fail;
}
'''
for name in ('BSP_BATTERY_CAL_RAW', 'BSP_BATTERY_CAL_MV'):
    source += re.search(r'^#define ' + name + r'\s+\d+U?', board, re.M)[0] + '\n'
source += implementation[implementation.index('esp_err_t bsp_battery_read_mv('):]
source += r'''
int main(void) {
    uint16_t mv=0,raw=0;
    assert(bsp_battery_read_mv(&mv,&raw)==ESP_OK && mv==3832 && raw==2752);
    sample=2400;assert(bsp_battery_read_mv(&mv,&raw)==ESP_OK && mv<3832);
    sample=3000;assert(bsp_battery_read_mv(&mv,&raw)==ESP_OK && mv>3832);
    assert(bsp_battery_read_mv(NULL,&raw)==ESP_ERR_INVALID_ARG);
    fail=ESP_ERR_INVALID_RESPONSE;
    assert(bsp_battery_read_mv(&mv,&raw)==ESP_ERR_INVALID_RESPONSE);
}
'''
cc = root / 'simulator/.tools/w64devkit/bin/gcc.exe'
env = dict(os.environ, PATH=str(cc.parent) + os.pathsep + os.environ['PATH'])
with tempfile.TemporaryDirectory() as directory:
    test, exe = Path(directory) / 'test.c', Path(directory) / 'test.exe'
    test.write_text(source)
    subprocess.run([str(cc), '-std=c11', '-Wall', '-Wextra', '-Werror', str(test), '-o', str(exe)], check=True, env=env)
    subprocess.run([str(exe)], check=True, env=env)
print('PASS: calibration point, voltage direction and ADC error propagation')
