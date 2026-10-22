"""Actual app scheduling: opt-in auto-save, result dwell, restart, duplicate suppression."""
from pathlib import Path
import os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
app=(root/'components/app/app_radio.c').read_text(encoding='utf-8')
source=r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rf_protocol.h"
typedef int lv_timer_t;
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_LOGW(...) ((void)0)
static uint8_t saved_receive_options, ui_options;
static unsigned auto_save_stage;
static int64_t auto_save_due, now;
static uint32_t auto_receive_frequency, last_auto_frequency, requested_hz;
static char last_auto_code[17];
static unsigned saves, resumes, option_writes;
static bool have_action;
static int write_error, request_error;
static rf_command_t queued;
static rf_status_t current;
static uint8_t rf_ui_get_receive_options(void){return ui_options;}
static void rf_ui_set_receive_options(uint8_t n){ui_options=n;}
static int user_receive_options_save(uint8_t n){(void)n;option_writes++;return write_error;}
static bool rf_ui_take_radio_action(rf_command_t *r){if(!have_action)return false;*r=queued;have_action=false;return true;}
static int rf_service_request(const rf_command_t *r){if(request_error)return request_error;if(r->type==RF_CMD_SAVE){assert(r->keep_result);saves++;}return 0;}
static void rf_service_status(rf_status_t *r){*r=current;}
static void rf_ui_update_radio(const rf_status_t *r){(void)r;}
static void rf_ui_radio_error(const char *s){(void)s;}
static const char *esp_err_to_name(int n){(void)n;return "error";}
static int64_t esp_timer_get_time(void){return now;}
static uint32_t rf_ui_receive_frequency(void){return requested_hz;}
static bool rf_ui_remote_receive(uint32_t hz,bool start){assert(start&&hz==requested_hz);resumes++;queued=(rf_command_t){.type=RF_CMD_RECEIVE,.frequency_hz=hz};have_action=true;return true;}
'''
source+=app[app.index('void app_radio_sync('):app.index('void app_radio_init(')]
source+=r'''
int main(void){
 current.state=RF_CAPTURED;current.frequency_hz=433920000;strcpy(current.code,"2D789C0555");
 app_radio_sync(NULL);assert(!saves); // Disabled by default.
 ui_options=1;app_radio_sync(NULL);assert(option_writes==1 && auto_save_stage==1);
 now=349999;app_radio_sync(NULL);assert(!saves);
 now++;app_radio_sync(NULL);assert(saves==1 && auto_save_stage==2);
 app_radio_sync(NULL);assert(saves==1); // No requeue while save is pending.
 current.state=RF_SAVED;app_radio_sync(NULL);now+=999999;app_radio_sync(NULL);assert(!resumes);
 now++;app_radio_sync(NULL);assert(resumes==1);current.state=RF_LISTENING;app_radio_sync(NULL);
 current.state=RF_CAPTURED;app_radio_sync(NULL);now+=350000;app_radio_sync(NULL);
 assert(saves==1 && resumes==2); // Same decoded held key doesn't fill flash.
 current.state=RF_LISTENING;app_radio_sync(NULL);strcpy(current.code,"2D789C0533");
 current.state=RF_CAPTURED;app_radio_sync(NULL);now+=350000;app_radio_sync(NULL);assert(saves==2);
 current.state=RF_FAILURE;app_radio_sync(NULL);assert(!auto_save_stage);
 ui_options=0;app_radio_sync(NULL);current.state=RF_CAPTURED;now+=9999999;app_radio_sync(NULL);assert(saves==2);
 write_error=7;ui_options=3;app_radio_sync(NULL);assert(saved_receive_options==0 && ui_options==0);
 write_error=0;ui_options=3;app_radio_sync(NULL);assert(saved_receive_options==3);
 // A user STOP cancels pending automatic work before it can queue a save.
 queued=(rf_command_t){.type=RF_CMD_STOP};have_action=true;now+=9999999;app_radio_sync(NULL);assert(saves==2 && !auto_save_stage);
 puts("PASS: opt-in persistence, delayed single save, result dwell/restart, held-key suppression, save failure and STOP");
}
'''
cc=root/'simulator/.tools/w64devkit/bin/gcc.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ['PATH'])
with tempfile.TemporaryDirectory() as d:
    test=Path(d)/'test.c';exe=Path(d)/'test.exe';test.write_text(source)
    subprocess.run([str(cc),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'components/rf_protocol/include'),str(test),'-o',str(exe)],check=True,env=env)
    subprocess.run([str(exe)],check=True,env=env)
