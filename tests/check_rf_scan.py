"""Execute actual CC1101 scan policy and tuning code with deterministic SPI/time stubs."""
from pathlib import Path
import os
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
service = (root / 'components/rf_service/rf_service.c').read_text(encoding='utf-8')
constants = service[service.index('#define SCAN_DWELL_US'):service.index('static const char *TAG')]
helpers = service[service.index('static bool supported_frequency('):service.index('static bool rx_done(')]
source = r'''
#include <stdint.h>
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include "rf_protocol.h"
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_ARG 1
#define ESP_ERR_NOT_FOUND 2
#define BSP_RADIO_XTAL_HZ 26000000
static int64_t now_us = 1000000, tuned_at;
static uint32_t tuned_frequency;
static bool rx_enabled;
static uint8_t regs[256];
static unsigned writes, calibrations, calibration_reads;
static bool fail_rx;
static int64_t esp_timer_get_time(void) { return now_us; }
static int write_reg(uint8_t a, uint8_t v) { regs[a]=v; writes++; return ESP_OK; }
static int bsp_radio_write_burst(uint8_t a, const uint8_t *v, size_t n) {
    memcpy(regs+a,v,n); writes++; return ESP_OK;
}
static int bsp_radio_read_burst(uint8_t a, uint8_t *v, size_t n) {
    assert(a==0x23 && n==3); calibration_reads++; memcpy(v,regs+a,n); return ESP_OK;
}
static int enter_state(uint8_t strobe, uint8_t expected) {
    (void)expected;
    if(strobe==0x34 || strobe==0x35) {
        if(fail_rx) return 9;
        if(regs[0x18]==0x18) {
            calibrations++; regs[0x23]=0xa0; regs[0x24]=0x21;
            regs[0x25]=regs[0x0f]; // Distinct per-frequency result for restoration tests.
        }
    }
    return ESP_OK;
}
'''
source += constants + helpers + r'''
static bool radio_ready=true, receiving, monitoring, have_candidate, auto_frequency, tx_enabled;
static int64_t last_rssi;
static unsigned samples, occupied, saved, disabled, reset_queues;
static int16_t peak_rssi;
static void *rx_channel, *tx_channel, *received, *paused;
static rf_state_t last_state;
static int save_error, arm_error;
static int command(uint8_t strobe) { assert(strobe==0x36); return 0; }
static int rmt_disable(void *channel) { (void)channel; disabled++; return 0; }
static void xQueueReset(void *queue) { (void)queue; reset_queues++; }
static void refresh_library(void) {}
static void publish(rf_state_t state, int error) { (void)error; last_state=state; }
static int xSemaphoreGive(void *semaphore) { (void)semaphore; return 1; }
static int user_signal_delete(uint32_t id) { (void)id; return 0; }
static int save_capture(void) { if(save_error) return save_error; assert(have_candidate); have_candidate=false; saved++; return 0; }
static int transmit(uint32_t id) { (void)id; return 0; }
static int rmt_enable(void *channel) { (void)channel; return 0; }
static int arm_receive(void) { return arm_error; }
'''
source += service[service.index('static void stop_radio('):service.index('static void refresh_library(')]
source += service[service.index('static void handle('):service.index('static void scan_tick(')]
source += r'''
int main(void) {
    assert(SCAN_COUNT==13);
    for(scan_index=0;scan_index<SCAN_COUNT;scan_index++) {
        uint32_t hz=scan_frequencies[scan_index];
        assert(supported_frequency(hz));
        assert((hz>=300000000 && hz<=348000000)||(hz>=387000000 && hz<=464000000));
    }
    assert(!supported_frequency(350000000));
    scan_channel_t a={0}, b={0};
    assert(!scan_trigger(&a,-110,now_us));
    assert(!scan_trigger(&a,-108,now_us));
    assert(scan_trigger(&a,-80,now_us));
    assert(!scan_trigger(&b,-80,now_us)); // No imaginary baseline on initial carrier.
    a.cooldown_until=now_us+SCAN_COOLDOWN_US;
    assert(!scan_trigger(&a,-75,now_us));
    assert(scan_trigger(&a,-75,a.cooldown_until));
    a.floor_dbm=-120;
    assert(!scan_trigger(&a,-95,now_us+SCAN_COOLDOWN_US)); // Absolute floor.
    a.floor_dbm=-80;
    assert(!scan_trigger(&a,-75,now_us+SCAN_COOLDOWN_US)); // Relative floor.
    a.floor_dbm=-80;
    scan_trigger(&a,-110,now_us);
    assert(a.floor_dbm < -88 && a.floor_dbm > -90); // Fast downward adaptation.
    // Baseline already contains a strong carrier: direct fallback must still arm.
    scan_channel_t busy={.baseline_valid=true,.floor_dbm=-75};
    scan_index=fallback_index=0;fallback_used=false;
    assert(scan_should_capture(&busy,-75,now_us));
    assert(fallback_used && fallback_index==1);
    scan_index=1; assert(!scan_should_capture(&busy,-75,now_us));
    fallback_used=false; assert(scan_should_capture(&busy,-75,now_us));
    assert(fallback_index==2);
    // A weak genuine burst also gets a capture opportunity without passing RSSI.
    scan_index=2;fallback_used=false;busy.floor_dbm=-110;
    assert(scan_should_capture(&busy,-110,now_us));
    memset(scan_channels,0,sizeof(scan_channels));
    scan_best=-1;scan_best_rssi=-127;fallback_used=true;
    bool lock_best=false;
    for(scan_index=0;scan_index<SCAN_COUNT;scan_index++) {
        // Strong 315 MHz interference must not preempt the stronger 433 remote.
        scan_consider(scan_index==0?-28:scan_index==1?-55:-110,now_us);
        if(scan_index<SCAN_COUNT-1) { assert(scan_next(false,true,&lock_best)==scan_index+1);assert(!lock_best); }
    }
    scan_index=SCAN_COUNT-1;assert(scan_next(false,true,&lock_best)==0 && lock_best);
    assert(scan_best==-1);
    tuned_at=now_us; rx_enabled=false;
    assert(!scan_due(now_us+1999)); assert(scan_due(now_us+2000));
    rx_enabled=true; locked_at=now_us; signal_at=now_us+10000;
    assert(!scan_due(now_us+100000)); assert(scan_due(signal_at+SCAN_HOLD_US));
    signal_at=now_us+999999;
    assert(!scan_due(now_us+999999)); assert(scan_due(now_us+SCAN_MAX_LOCK_US));
    // First tune calibrates; subsequent hops restore the correct channel's cached result.
    assert(tune(433920000,false)==ESP_OK);
    assert(calibrations==1 && calibration_reads==1);
    uint8_t first_cal=regs[0x25]; unsigned first_writes=writes;
    now_us+=10000;
    assert(tune(315000000,false)==ESP_OK); assert(calibrations==2);
    unsigned before=writes;
    assert(tune(433920000,false)==ESP_OK);
    assert(calibrations==2 && regs[0x25]==first_cal && regs[0x18]==0x08);
    assert(writes-before < first_writes); // No full profile reload on each hop.
    now_us+=CAL_MAX_AGE_US;
    assert(tune(433920000,false)==ESP_OK); assert(calibrations==3);
    assert(tune(315000000,true)==ESP_OK); assert(calibrations==4 && regs[0x3f]==0x51);
    before=writes; assert(tune(350000000,false)==ESP_ERR_INVALID_ARG); assert(writes==before);
    fail_rx=true;
    assert(tune(433920000,false)!=ESP_OK); assert(!profile_loaded);
    for(unsigned i=0;i<SCAN_COUNT;i++) assert(!scan_channels[i].calibration_valid);
    fail_rx=false;
    assert(tune(433920000,false)==ESP_OK); assert(profile_loaded);
    rf_command_t request={.type=RF_CMD_RECEIVE,.frequency_hz=0};
    handle(&request); assert(receiving && !rx_enabled && auto_frequency && last_state==RF_LISTENING);
    rx_enabled=true; have_candidate=true; // A locked capture is interrupted by STOP/OTA.
    request.type=RF_CMD_STOP; handle(&request);
    assert(!receiving && !monitoring && !rx_enabled && disabled && last_state==RF_IDLE);
    // Auto save resumes probe-only scanning; fixed-frequency save remains on result screen.
    request.type=RF_CMD_SAVE; handle(&request);
    assert(saved==1 && receiving && !rx_enabled && last_state==RF_LISTENING);
    receiving=false; have_candidate=true; save_error=3; handle(&request);
    assert(!receiving && have_candidate && last_state==RF_FAILURE); save_error=0;
    request.type=RF_CMD_RECEIVE; request.frequency_hz=315000000; handle(&request);
    assert(receiving && rx_enabled && !auto_frequency && scan_index==1);
    receiving=false; have_candidate=true; request.type=RF_CMD_SAVE; handle(&request);
    assert(last_state==RF_SAVED && !receiving);
    request.type=RF_CMD_RECEIVE; request.frequency_hz=350000000; handle(&request);
    assert(last_state==RF_FAILURE && !receiving && !rx_enabled);
    arm_error=4; request.type=RF_CMD_RECEIVE; request.frequency_hz=315000000; handle(&request);
    assert(last_state==RF_FAILURE && !receiving && !rx_enabled); arm_error=0;
    request.type=RF_CMD_MONITOR; request.frequency_hz=0; handle(&request);
    assert(monitoring && !receiving && !rx_enabled);
    puts("PASS: scan baseline/threshold/cooldown, lock limits, calibration cache/expiry/failure, stop, save/resume, fixed RX, monitor, RMT failure, TX");
}
'''
compiler = root / 'simulator/.tools/w64devkit/bin/gcc.exe'
env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ['PATH'])
with tempfile.TemporaryDirectory(prefix='rf-scan-') as directory:
    test = Path(directory) / 'test.c'
    binary = Path(directory) / 'check.exe'
    test.write_text(source, encoding='utf-8')
    subprocess.run([str(compiler), '-std=c11', '-Wall', '-Wextra', '-Werror', '-I'+str(root/'components/rf_protocol/include'), str(test), '-o', str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)
