"""Regression: actual capture acceptance must reject isolated/oversize RAW noise."""
from pathlib import Path
import os, subprocess, tempfile, json
root=Path(__file__).resolve().parents[1]
s=(root/'components/rf_service/rf_service.c').read_text(encoding='utf-8')
source=r'''
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "rf_protocol.h"
#define RX_SYMBOLS 128
#define RX_IDLE_US 32000
static uint32_t rx_blocks, rx_full_blocks, rx_decoded_blocks;
#define portMAX_DELAY 0
#define ESP_OK 0
static struct {unsigned duration0,duration1,level0,level1;} rx_symbols[RX_SYMBOLS];
#define rx_snapshot rx_symbols
static rf_frame_t storage[2], *capture=&storage[0], *candidate=&storage[1];
static bool rx_overflow, rx_snapshot_partial, have_candidate, receiving=true;
static uint32_t tuned_frequency=315000000;
static int16_t peak_rssi=-70;
static int64_t signal_at;
static rf_status_t status;
static void *status_lock;
static int64_t esp_timer_get_time(void) {return 100000;}
static void xSemaphoreTake(void *s,int t) {(void)s;(void)t;}
static void xSemaphoreGive(void *s) {(void)s;}
static void stop_radio(void) {receiving=false;}
static void publish(rf_state_t s,int e) {(void)e;status.state=s;}
'''
source+=s[s.index('static void read_capture('):s.index('static void handle(')]
measured=json.loads((root/'tests/fixtures/rx433_captured_raw.json').read_text())
assert measured['first_level']==1
source+='static const uint16_t measured[] = {'+','.join(map(str,measured['duration']))+'};\n'
source+=r'''
int main(void) {
    for(unsigned i=0;i<10;i++) {
        rx_symbols[i].duration0=500;rx_symbols[i].duration1=500;
        rx_symbols[i].level0=1;rx_symbols[i].level1=0;
    }
    read_capture(10);
    assert(receiving && have_candidate && status.state!=RF_CAPTURED);
    have_candidate=false; rx_snapshot_partial=true;
    read_capture(10); // A short partial callback is still truncated RAW.
    assert(receiving && !have_candidate);
    rx_snapshot_partial=false; read_capture(10);
    peak_rssi=-127; read_capture(10); // Historical peak cannot validate a weak next capture.
    assert(receiving && status.state!=RF_CAPTURED);
    peak_rssi=-70; read_capture(10);
    assert(!receiving && status.state==RF_CAPTURED && status.raw);
    receiving=true; have_candidate=false;status.state=RF_LISTENING;
    for(unsigned i=0;i<20;i++) rx_symbols[i]=rx_symbols[0];
    for(unsigned i=0;i<20;i++) rx_symbols[i].duration0=rx_symbols[i].duration1=10000;
    read_capture(20); // 400 ms random RAW: discard, never fail the receiver.
    assert(receiving && !have_candidate && status.state==RF_LISTENING);
    read_capture(RX_SYMBOLS);
    assert(receiving && status.state==RF_LISTENING);
    rf_frame_t frame={0}; assert(rf_frame_encode(&frame,0x123456,24,350));
    for(unsigned i=0;i<frame.count/2;i++) {
        rx_symbols[i].duration0=frame.duration[2*i];rx_symbols[i].duration1=frame.duration[2*i+1];
        rx_symbols[i].level0=1;rx_symbols[i].level1=0;
    }
    read_capture(frame.count/2); assert(receiving);
    read_capture(frame.count/2); assert(!receiving && !status.raw && status.bits==24);
    receiving=true;have_candidate=false;status.state=RF_LISTENING;
    assert(rf_frame_encode(&frame,0xabcdef,24,500)); // 15.5 ms sync must survive.
    for(unsigned i=0;i<RX_SYMBOLS;i++) {
        unsigned p=(i*2)%frame.count;
        rx_symbols[i].duration0=frame.duration[p];rx_symbols[i].duration1=frame.duration[p+1];
        rx_symbols[i].level0=1;rx_symbols[i].level1=0;
    }
    read_capture(RX_SYMBOLS);
    assert(!receiving && !status.raw && status.bits==24 && candidate->code==0xabcdef);
    receiving=true;have_candidate=false;status.state=RF_LISTENING;
    const unsigned raw_count=sizeof(measured)/sizeof(measured[0]);
    for(unsigned i=0;i<RX_SYMBOLS;i++) {
        rx_symbols[i].duration0=measured[(2*i)%raw_count];
        rx_symbols[i].duration1=measured[(2*i+1)%raw_count];
        rx_symbols[i].level0=1;rx_symbols[i].level1=0;
    }
    rx_snapshot_partial=true;
    read_capture(RX_SYMBOLS);
    assert(!receiving && status.raw && candidate->count==raw_count && candidate->repeats>=2);
    assert(strlen(status.code)==10 && strncmp(status.code,"RAW",3));
    assert(!candidate->tail_clipped && candidate->first_level==1);
    assert(!memcmp(candidate->duration,measured,sizeof(measured)));
    receiving=true;have_candidate=false;status.state=RF_LISTENING;
    // A changed pulse in the second complete frame must break confirmation.
    rx_symbols[raw_count+4].duration0=2000;
    rx_symbols[raw_count/2+4].duration0=2000;
    read_capture(RX_SYMBOLS);
    assert(receiving && have_candidate && status.state==RF_LISTENING);
    puts("PASS: isolated/weak/oversize/full RAW rejected, repeated RAW and repeated fixed code accepted");
}
'''
compiler=root/'simulator/.tools/w64devkit/bin/gcc.exe'
env=dict(os.environ,PATH=str(compiler.parent)+os.pathsep+os.environ['PATH'])
with tempfile.TemporaryDirectory(prefix='rf-capture-') as d:
    test=Path(d)/'test.c';binary=Path(d)/'test.exe';test.write_text(source,encoding='utf-8')
    subprocess.run([str(compiler),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'components/rf_protocol/include'),str(test),str(root/'components/rf_protocol/rf_protocol.c'),'-o',str(binary)],check=True,env=env)
    subprocess.run([str(binary)],check=True,env=env)
