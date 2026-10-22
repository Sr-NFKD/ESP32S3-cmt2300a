"""Actual USB command parser and UI navigation, with transport/LVGL stubs."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
app=(root/'components/app/app_console.c').read_text(encoding='utf-8')
ui=(root/'components/rf_ui/rf_ui.c').read_text(encoding='utf-8')
source=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rf_protocol.h"
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
#define TRANSITION_HORIZONTAL 1
#define VIEW_CAPTURE 2
#define VIEW_RECEIVE 3
typedef int lv_timer_t;
static unsigned frequency_page,page,repeat_key,view;
static bool radio_details;
static const uint32_t frequencies[]={0,433920000,315000000,318000000};
#define FREQUENCY_COUNT 4
static unsigned actions, transitions, dispatches;
static rf_command_type_t action;
static void request_radio(rf_command_type_t t){action=t;actions++;}
static void change_view(int a,int b){assert(a==1&&b==1);transitions++;}
static void app_radio_sync(void *p){(void)p;dispatches++;}
static void rf_service_status(rf_status_t *s){memset(s,0,sizeof(*s));}
static int64_t last_activity;
static void app_power_activity(void){last_activity=123;}
static const char *input;
static int usb_serial_jtag_read_bytes(void *buf,unsigned len,int ticks){
    assert(ticks==0);unsigned n=strlen(input);if(n>len)n=len;
    memcpy(buf,input,n);input+=n;return (int)n;
}
'''
start=ui.index('bool rf_ui_remote_receive(')
source+=ui[start:ui.index('static bool radio_key(', start)]
source+=app[app.index('static void serial_command('):len(app)]
source+=r'''
static void send(const char *s){input=s;do{app_console_poll(NULL);}while(*input);}
int main(void){
    send("rx 4");assert(!actions);send("33\r\n");
    assert(actions==1 && dispatches==1 && transitions==1 && frequency_page==1);
    assert(view==VIEW_CAPTURE && action==RF_CMD_RECEIVE && last_activity==123);
    send("stop\nrx auto\nrx 315\nrx 318\n");assert(actions==5 && frequency_page==3);
    send("status\ninvalid\n");assert(actions==5);
    send("rx 433xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\n");
    assert(actions==5);send("rx 433\n");assert(actions==6 && frequency_page==1);
    send("stop\n");assert(action==RF_CMD_STOP && view==VIEW_RECEIVE);
    assert(!rf_ui_remote_receive(123,true));assert(actions==7);
    puts("PASS: fragmented/CRLF/multiple/oversize USB commands and matching UI/RF actions");
}
'''
compiler=root/'simulator/.tools/w64devkit/bin/gcc.exe'
env=dict(os.environ,PATH=str(compiler.parent)+os.pathsep+os.environ['PATH'])
with tempfile.TemporaryDirectory(prefix='rf-serial-') as d:
    test=Path(d)/'test.c';binary=Path(d)/'test.exe';test.write_text(source,encoding='utf-8')
    subprocess.run([str(compiler),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'components/rf_protocol/include'),str(test),'-o',str(binary)],check=True,env=env)
    subprocess.run([str(binary)],check=True,env=env)
