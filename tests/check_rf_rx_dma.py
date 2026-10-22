"""Exercise actual RMT callback, arming and worker completion path with DMA stubs."""
from pathlib import Path
import os, subprocess, tempfile
root = Path(__file__).resolve().parents[1]
s = (root/'components/rf_service/rf_service.c').read_text(encoding='utf-8')
assert '#define RX_SYMBOLS 128' in s
assert 'heap_caps_calloc(RF_MAX_PULSES / 2 + 1' in s  # RX sizing must not shrink TX.
source = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdatomic.h>
#include <string.h>
#include <stdio.h>
#define RX_SYMBOLS 128
#define RX_DMA_SYMBOLS (RX_SYMBOLS * 2)
#define RX_IDLE_US 32000
#define pdFALSE 0
#define pdTRUE 1
#define ESP_OK 0
#define RF_FAILURE 9
typedef int BaseType_t;
typedef int esp_err_t;
typedef void *rmt_channel_handle_t;
typedef uint32_t rmt_symbol_word_t;
typedef struct {rmt_symbol_word_t *received_symbols; size_t num_symbols;
    struct {bool is_last;} flags;} rmt_rx_done_event_data_t;
typedef struct {unsigned signal_range_min_ns, signal_range_max_ns;
    struct {bool en_partial_rx;} flags;} rmt_receive_config_t;
static rmt_symbol_word_t rx_symbols[RX_DMA_SYMBOLS], rx_snapshot[RX_SYMBOLS];
static bool rx_snapshot_partial;
static atomic_bool rx_event_claimed;
static bool rx_overflow, rx_enabled=true, receiving=true, queued, running;
static void *received, *rx_channel, *worker_task=(void*)1;
static size_t queued_count;
static int peak_rssi, disabled, armed, analyzed;
static int xQueueSendFromISR(void *q, const size_t *n, int *wake) {
    (void)q; *wake=1; assert(!queued); queued=true; queued_count=*n; return 1;
}
static void vTaskNotifyGiveFromISR(void *t, int *wake) {(void)t; *wake=1;}
static int xQueueReceive(void *q,size_t *n,int wait) {
    (void)q;(void)wait;if(!queued)return 0;*n=queued_count;queued=false;return 1;
}
static void xQueueReset(void *q) {(void)q;queued=false;}
static int rmt_disable(void *c) {(void)c;assert(rx_enabled);running=false;disabled++;return 0;}
static int rmt_enable(void *c) {(void)c;assert(!rx_enabled);return 0;}
static int rmt_receive(void *c,void *buf,size_t len,const rmt_receive_config_t *cfg) {
    (void)c;assert(!running);assert(buf==rx_symbols && len==sizeof(rx_symbols));
    assert(cfg->flags.en_partial_rx && cfg->signal_range_max_ns==32000000);
    running=true;armed++;return 0;
}
static void stop_radio(void) {receiving=false;rx_enabled=false;running=false;}
static void publish(int state,int err) {(void)state;(void)err;assert(0);}
static void log_capture(size_t n) {(void)n;}
static void read_capture(size_t n) {
    assert(!running && !rx_enabled);assert(n==RX_SYMBOLS);
    for(unsigned i=0;i<n;i++)assert(rx_snapshot[i]==i+1);
    analyzed++;
}
'''
source += s[s.index('static bool rx_done('):s.index('static void stop_radio(')]
start = s.index('        if (receiving && rx_enabled && xQueueReceive(received, &symbols')
end = s.index('        int64_t now =', start)
source += 'static void tick(void) {for(int once=0;once<1;once++){size_t symbols;\n' + s[start:end] + '}}\n'
source += r'''
int main(void) {
    assert(arm_receive()==0);
    for(unsigned i=0;i<RX_SYMBOLS;i++)rx_symbols[i]=i+1;
    rmt_rx_done_event_data_t event={.received_symbols=rx_symbols,.num_symbols=RX_SYMBOLS};
    assert(rx_done(NULL,&event,NULL)); // Full DMA block, no idle/end marker.
    assert(rx_snapshot_partial);
    memset(rx_symbols,0,sizeof(rx_symbols)); // DMA source is reused after callback.
    event.flags.is_last=true;
    assert(!rx_done(NULL,&event,NULL)); // Late final event cannot overwrite snapshot.
    tick();assert(analyzed==1 && disabled==1 && armed==2 && running && !queued);
    for(unsigned i=0;i<RX_SYMBOLS;i++)rx_symbols[i]=i+1;
    running=false; // Normal EOF has already stopped RMT in the real driver.
    assert(rx_done(NULL,&event,NULL));assert(!rx_snapshot_partial);tick();
    assert(analyzed==2 && disabled==2 && armed==3 && !rx_event_claimed);
    puts("PASS: partial DMA snapshot, duplicate event suppression, stop/enable/rearm, normal EOF");
}
'''
compiler=root/'simulator/.tools/w64devkit/bin/gcc.exe'
env=dict(os.environ,PATH=str(compiler.parent)+os.pathsep+os.environ['PATH'])
with tempfile.TemporaryDirectory(prefix='rf-dma-') as d:
    test=Path(d)/'test.c'; binary=Path(d)/'test.exe'
    test.write_text(source,encoding='utf-8')
    subprocess.run([str(compiler),'-std=c11','-Wall','-Wextra','-Werror',str(test),'-o',str(binary)],check=True,env=env)
    subprocess.run([str(binary)],check=True,env=env)
