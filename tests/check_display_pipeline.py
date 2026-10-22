"""Exercise the real display flush/wait/ISR functions with mocked SPI and DMA.

Checks buffer ownership and completion races; does not measure physical FPS.
"""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "components/bsp/bsp_display.c").read_text(encoding="utf-8")
callbacks = source[source.index("static bool on_color_done("):source.index("static void read_keypad(")]
panel = source[source.index("typedef struct { uint8_t cmd,"):source.index("static bool on_color_done(")]
panel += source[source.index("static esp_err_t panel_start("):source.index("esp_err_t bsp_display_init(")]
sleep = source[source.index("esp_err_t bsp_display_sleep("):source.index("lv_display_t *bsp_display_get(")]
prefix = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
typedef int esp_err_t;
typedef void *esp_lcd_panel_io_handle_t;
typedef void esp_lcd_panel_io_event_data_t;
typedef int BaseType_t;
typedef void *SemaphoreHandle_t;
typedef struct {int ready;} lv_display_t;
typedef struct {int x1,y1,x2,y2;} lv_area_t;
#define ESP_OK 0
#define pdFALSE 0
#define pdTRUE 1
#define pdMS_TO_TICKS(n) (n)
#define BSP_LCD_X_OFFSET 1
#define BSP_LCD_Y_OFFSET 26
#define BSP_LCD_RST 14
#define BSP_LCD_MADCTL 0xA8
#define ESP_LOGE(...) ((void)0)
static esp_lcd_panel_io_handle_t io;
static SemaphoreHandle_t color_done;
static bool transfer_pending;
static lv_display_t *display;
static int commands[128], command_count, elapsed, reset_count;
static int gpio_set_level(int pin,int level){assert(pin==14);assert(level==reset_count%2);reset_count++;return 0;}
static void vTaskDelay(int ms){elapsed+=ms;}
static int tokens, takes, writes, error_command, early_completion;
static int64_t esp_timer_get_time(void){return 1000;}
static int xSemaphoreGiveFromISR(void *s,int *wake){(void)s;tokens++;*wake=1;return 1;}
static int xSemaphoreTake(void *s,int timeout){(void)s;(void)timeout;takes++;assert(tokens==1);tokens--;return 1;}
static void lv_display_flush_ready(lv_display_t *d){d->ready++;}
static void lv_draw_sw_rgb565_swap(uint8_t *pixels,uint32_t count){(void)pixels;assert(count==3200);}
static esp_err_t esp_lcd_panel_io_tx_param(void *p,int cmd,const void *data,size_t n){
    (void)p;(void)data;assert(n<=16);commands[command_count++]=cmd;return error_command==cmd?42:ESP_OK;
}
static esp_err_t esp_lcd_panel_io_tx_color(void *,int,const void *,size_t);
'''
suffix = r'''
static esp_err_t esp_lcd_panel_io_tx_color(void *p,int cmd,const void *pixels,size_t n){
    (void)p;(void)pixels;assert(cmd==0x2c);assert(n==6400);writes++;
    if(error_command==cmd)return 42;
    if(early_completion)assert(on_color_done(NULL,NULL,color_done));
    return ESP_OK;
}
int main(void){
    lv_display_t d={0};lv_area_t area={0,0,159,19};uint8_t pixels[6400];
    assert(tick_ms()==1);wait_flush(&d);assert(takes==0);
    flush(&d,&area,pixels);
    assert(transfer_pending&&writes==1&&takes==0&&d.ready==0); // Rendering can continue.
    assert(on_color_done(NULL,NULL,color_done));assert(tokens==1);
    wait_flush(&d);assert(!transfer_pending&&tokens==0&&takes==1);
    wait_flush(&d);assert(takes==1); // Never wait twice for the same DMA.
    early_completion=1;flush(&d,&area,pixels);assert(transfer_pending&&tokens==1);
    wait_flush(&d);assert(!transfer_pending&&tokens==0&&takes==2); // ISR before submit returns.
    early_completion=0;
    for(int i=0;i<3;i++){
        const int commands[]={0x2a,0x2b,0x2c};error_command=commands[i];
        flush(&d,&area,pixels);assert(!transfer_pending&&tokens==0&&d.ready==i+1);
        wait_flush(&d);assert(takes==2); // Failed submit cannot leave LVGL waiting forever.
    }
    error_command=0; display=&d; command_count=0;
    early_completion=1;flush(&d,&area,pixels);
    assert(transfer_pending && tokens==1);
    command_count=0;
    assert(bsp_display_sleep(true)==ESP_OK);
    assert(!transfer_pending && !tokens && commands[0]==0x28 && commands[1]==0x10 && elapsed==120);
    for(int cycle=0;cycle<3;cycle++){
        command_count=reset_count=elapsed=0;
        assert(bsp_display_sleep(false)==ESP_OK);
        assert(reset_count==2 && elapsed==250 && commands[0]==0x11);
        assert(command_count==1+(int)(sizeof(init_commands)/sizeof(init_commands[0])));
        for(int i=1;i<command_count;i++)assert(commands[i]==init_commands[i-1].cmd);
        assert(commands[command_count-1]==0x29); // Display-on after full panel configuration.
        command_count=0;flush(&d,&area,pixels);wait_flush(&d);assert(!transfer_pending && !tokens);
    }
    command_count=reset_count=elapsed=0;error_command=0x11;
    assert(bsp_display_sleep(false)==42 && command_count==1); // Never hide a failed recovery.
    puts("PASS: DMA ownership/errors, sleep drains DMA, repeated full panel wake and recovery failure");
}
'''
compiler = root / "simulator/.tools/w64devkit/bin/gcc.exe"
env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ["PATH"])
with tempfile.TemporaryDirectory(prefix="rf-display-pipeline-") as directory:
    test = Path(directory) / "test.c"
    test.write_text(prefix + callbacks + panel + sleep + suffix, encoding="utf-8")
    binary = Path(directory) / "check.exe"
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror", str(test), "-o", str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)
