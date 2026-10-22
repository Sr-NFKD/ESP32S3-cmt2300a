"""Compile the real user_data.c with SDK cJSON and an in-memory NVS substitute.

Checks data semantics, not physical flash/power-loss behavior. No device required.
"""
from pathlib import Path
import os
import subprocess
import tempfile
import json

root = Path(__file__).resolve().parents[1]
sdk = Path(os.environ.get("IDF_PATH", "A:/Espressif-IDF/v5.5.4/esp-idf"))
compiler = root / "simulator/.tools/w64devkit/bin/gcc.exe"
env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ["PATH"])

headers = {
    "esp_err.h": """#pragma once
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_NO_MEM 1
#define ESP_ERR_INVALID_ARG 2
#define ESP_ERR_INVALID_STATE 3
#define ESP_ERR_INVALID_SIZE 4
#define ESP_ERR_INVALID_RESPONSE 5
#define ESP_ERR_NOT_FOUND 6
#define ESP_ERR_NVS_NOT_FOUND 7
#define ESP_ERR_NOT_SUPPORTED 8
""",
    "nvs_flash.h": '#include "esp_err.h"\nesp_err_t nvs_flash_init(void);\nesp_err_t nvs_flash_init_partition(const char *);\n',
    "esp_heap_caps.h": '#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\nstatic inline void *heap_caps_malloc(size_t n,int caps){(void)caps;return malloc(n); }\n',
    "freertos/FreeRTOS.h": '#pragma once\n#define portMAX_DELAY 0\n',
    "freertos/semphr.h": """#pragma once
typedef void *SemaphoreHandle_t;
static inline void *xSemaphoreCreateMutex(void){return (void *)1;}
static inline int xSemaphoreTake(void *p,int t){(void)p;(void)t;return 1;}
static inline int xSemaphoreGive(void *p){(void)p;return 1;}
""",
    "nvs.h": """#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef int nvs_handle_t;
typedef int nvs_open_mode_t;
#define NVS_READONLY 0
#define NVS_READWRITE 1
esp_err_t nvs_open(const char *,int,nvs_handle_t *);
esp_err_t nvs_open_from_partition(const char *,const char *,int,nvs_handle_t *);
esp_err_t nvs_get_blob(nvs_handle_t,const char *,void *,size_t *);
esp_err_t nvs_set_blob(nvs_handle_t,const char *,const void *,size_t);
void nvs_close(nvs_handle_t);
esp_err_t nvs_get_str(nvs_handle_t,const char *,char *,size_t *);
esp_err_t nvs_set_str(nvs_handle_t,const char *,const char *);
esp_err_t nvs_commit(nvs_handle_t);
esp_err_t nvs_get_u8(nvs_handle_t,const char *,uint8_t *);
esp_err_t nvs_set_u8(nvs_handle_t,const char *,uint8_t);
esp_err_t nvs_get_u32(nvs_handle_t,const char *,uint32_t *);
esp_err_t nvs_set_u32(nvs_handle_t,const char *,uint32_t);
esp_err_t nvs_erase_all(nvs_handle_t);
esp_err_t nvs_erase_key(nvs_handle_t,const char *);
""",
}

source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "user_data.h"
#include "nvs.h"
typedef struct { char records[8192], library[49152]; uint32_t next_id; uint8_t brightness, receive_options; bool has_options; uint32_t pending; int has_pending; uint32_t sleep_minutes; int has_sleep; } Store;
static Store disk[4], work[4];
static int fail_commit = -1;
esp_err_t nvs_flash_init(void){return ESP_OK;}
esp_err_t nvs_flash_init_partition(const char *name){(void)name;return ESP_OK;}
esp_err_t nvs_open(const char *name,int mode,nvs_handle_t *h){
  (void)mode; *h=!strcmp(name,"signals")?0:!strcmp(name,"settings")?1:2;
  work[*h]=disk[*h];return ESP_OK;
}
esp_err_t nvs_open_from_partition(const char *p,const char *n,int m,nvs_handle_t *h){(void)p;(void)n;(void)m;*h=3;work[3]=disk[3];return ESP_OK;}
void nvs_close(nvs_handle_t h){(void)h;}
esp_err_t nvs_get_blob(nvs_handle_t h,const char *k,void *s,size_t *n){(void)k;if(!disk[h].library[0])return ESP_ERR_NVS_NOT_FOUND;if(!s){*n=strlen(disk[h].library)+1;return ESP_OK;}assert(*n>=strlen(disk[h].library)+1);memcpy(s,disk[h].library,strlen(disk[h].library)+1);return ESP_OK;}
esp_err_t nvs_set_blob(nvs_handle_t h,const char *k,const void *s,size_t n){(void)k;assert(n<=sizeof(work[h].library));memcpy(work[h].library,s,n);return ESP_OK;}
esp_err_t nvs_get_str(nvs_handle_t h,const char *k,char *s,size_t *n){
  (void)k;if(!disk[h].records[0])return ESP_ERR_NVS_NOT_FOUND;
  if(!s){*n=strlen(disk[h].records)+1;return ESP_OK;}
  assert(*n>=strlen(disk[h].records)+1);strcpy(s,disk[h].records);return ESP_OK;
}
esp_err_t nvs_set_str(nvs_handle_t h,const char *k,const char *s){(void)k;strcpy(work[h].records,s);return ESP_OK;}
esp_err_t nvs_commit(nvs_handle_t h){if(fail_commit==h){fail_commit=-1;return 99;}disk[h]=work[h];return ESP_OK;}
esp_err_t nvs_get_u8(nvs_handle_t h,const char *k,uint8_t *v){if(!strcmp(k,"receive_options")){if(!disk[h].has_options)return ESP_ERR_NVS_NOT_FOUND;*v=disk[h].receive_options;return ESP_OK;}if(!disk[h].brightness)return ESP_ERR_NVS_NOT_FOUND;*v=disk[h].brightness;return ESP_OK;}
esp_err_t nvs_set_u8(nvs_handle_t h,const char *k,uint8_t v){if(!strcmp(k,"receive_options")){work[h].receive_options=v;work[h].has_options=true;return ESP_OK;}work[h].brightness=v;return ESP_OK;}
esp_err_t nvs_get_u32(nvs_handle_t h,const char *k,uint32_t *v){if(!strcmp(k,"sleep_minutes")){if(!disk[h].has_sleep)return ESP_ERR_NVS_NOT_FOUND;*v=disk[h].sleep_minutes;return ESP_OK;}if(!strcmp(k,"next_id")){if(!disk[h].next_id)return ESP_ERR_NVS_NOT_FOUND;*v=disk[h].next_id;return ESP_OK;}if(!disk[h].has_pending)return ESP_ERR_NVS_NOT_FOUND;*v=disk[h].pending;return ESP_OK;}
esp_err_t nvs_set_u32(nvs_handle_t h,const char *k,uint32_t v){if(!strcmp(k,"sleep_minutes")){work[h].sleep_minutes=v;work[h].has_sleep=1;return ESP_OK;}if(!strcmp(k,"next_id")){work[h].next_id=v;return ESP_OK;}work[h].pending=v;work[h].has_pending=1;return ESP_OK;}
esp_err_t nvs_erase_all(nvs_handle_t h){memset(&work[h],0,sizeof(Store));return ESP_OK;}
esp_err_t nvs_erase_key(nvs_handle_t h,const char *k){(void)k;work[h].has_pending=0;return ESP_OK;}
static int count(void){cJSON *a=NULL;assert(user_signals_read(&a)==ESP_OK);int n=cJSON_GetArraySize(a);cJSON_Delete(a);return n;}
static uint32_t save(void){
  cJSON *r=cJSON_Parse("{\"name\":\"Door\",\"key_code\":\"000ABC\",\"frequency_hz\":433920000,\"raw\":[100,200]}");
  uint32_t id=0;assert(user_signal_save(r,&id)==ESP_OK);cJSON_Delete(r);return id;
}
int main(void){
  assert(user_data_init()==ESP_OK);assert(count()==0);assert(user_brightness_load()==70);
  assert(user_receive_options_load()==0);
  for(unsigned option=0;option<4;option++){assert(user_receive_options_save(option)==ESP_OK);assert(user_receive_options_load()==option);}
  fail_commit=1;assert(user_receive_options_save(0)!=ESP_OK);assert(user_receive_options_load()==3);
  assert(user_receive_options_save(4)==ESP_ERR_INVALID_ARG);
  assert(user_receive_options_save(0)==ESP_OK);
  uint8_t light, deep; user_sleep_load(&light,&deep); assert(light==10 && deep==20);
  assert(user_sleep_save(3,7)==ESP_OK); user_sleep_load(&light,&deep); assert(light==3 && deep==7);
  fail_commit=1; assert(user_sleep_save(5,9)!=ESP_OK);
  user_sleep_load(&light,&deep); assert(light==3 && deep==7);
  assert(user_sleep_save(0,0)==ESP_OK); user_sleep_load(&light,&deep); assert(!light && !deep);
  assert(user_sleep_save(121,20)==ESP_ERR_INVALID_ARG);
  assert(user_sleep_save(10,121)==ESP_ERR_INVALID_ARG);
  disk[1].sleep_minutes=0xffffffff; user_sleep_load(&light,&deep); assert(light==10 && deep==20);
  uint32_t id=save();assert(id==1);assert(count()==1);
  assert(user_signal_edit(id,"Bell","0000FF")==ESP_OK);
  cJSON *a=NULL;assert(user_signals_read(&a)==ESP_OK);
  cJSON *r=cJSON_GetArrayItem(a,0);
  assert(!strcmp(cJSON_GetObjectItem(r,"key_code")->valuestring,"0000FF"));
  assert(cJSON_GetObjectItem(r,"frequency_hz")->valueint==433920000);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(r,"raw"))==2);cJSON_Delete(a);
  assert(user_signal_edit(id,"Bad","XYZ")==ESP_ERR_INVALID_ARG);
  assert(user_signal_edit(id,"","123")==ESP_ERR_INVALID_ARG);
  assert(user_signal_delete(999)==ESP_ERR_NOT_FOUND);
  fail_commit=3;assert(user_signal_delete(id)!=ESP_OK);assert(count()==1);
  assert(user_signal_delete(id)==ESP_OK);assert(count()==0);
  assert(save()>id); // IDs never alias a previously deleted record.
  memset(disk,0,sizeof(disk));
  strcpy(disk[0].records,"[{\"id\":7,\"name\":\"Legacy\",\"key_code\":\"00FF\"}]");
  assert(count()==1);assert(user_signal_edit(7,"Migrated","00AA")==ESP_OK);
  assert(disk[3].library[0]);assert(user_signal_delete(7)==ESP_OK);assert(count()==0);
  memset(disk,0,sizeof(disk));
  cJSON *raw=cJSON_Parse("{\"name\":\"Raw\",\"key_code\":\"0\",\"protocol\":\"RAW\",\"raw\":[100,200]}");
  uint32_t raw_id;assert(user_signal_save(raw,&raw_id)==ESP_OK);cJSON_Delete(raw);
  assert(user_signal_edit(raw_id,"Raw renamed","1")==ESP_ERR_NOT_SUPPORTED);
  assert(user_signal_edit(raw_id,"Raw renamed","0")==ESP_OK);
  memset(disk,0,sizeof(disk));
  cJSON *pwm=cJSON_Parse("{\"name\":\"PWM\",\"key_code\":\"000001\",\"protocol\":\"OOK_PWM\",\"bits\":24,\"pulse_us\":350,\"raw\":[1,2]}");
  uint32_t pwm_id;assert(user_signal_save(pwm,&pwm_id)==ESP_OK);cJSON_Delete(pwm);
  assert(user_signal_edit(pwm_id,"PWM","FFFFFFF")==ESP_ERR_INVALID_ARG);
  assert(user_signal_edit(pwm_id,"PWM","000002")==ESP_OK);
  assert(user_signals_read(&a)==ESP_OK);r=cJSON_GetArrayItem(a,0);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(r,"raw"))==50);cJSON_Delete(a);
  memset(disk,0,sizeof(disk));
  cJSON *large=cJSON_Parse("{\"name\":\"Long raw\",\"key_code\":\"0\",\"protocol\":\"RAW\"}");
  cJSON *durations=cJSON_CreateArray();
  for(int i=0;i<1000;i++)cJSON_AddItemToArray(durations,cJSON_CreateNumber(12345));
  cJSON_AddItemToObject(large,"raw",durations);uint32_t large_id;
  assert(user_signal_save(large,&large_id)==ESP_OK);cJSON_Delete(large);
  assert(strlen(disk[3].library)>4000);assert(user_signals_read(&a)==ESP_OK);
  assert(cJSON_GetArraySize(cJSON_GetObjectItem(cJSON_GetArrayItem(a,0),"raw"))==1000);cJSON_Delete(a);
  for(int retain=0;retain<2;retain++)for(int reset=0;reset<2;reset++){
    memset(disk,0,sizeof(disk));save();assert(user_brightness_save(23)==ESP_OK);
    assert(user_upgrade_stage(0x11,retain,reset)==ESP_OK);
    assert(count()==1);assert(user_brightness_load()==23); // No pre-boot erasure.
    assert(user_upgrade_finish(0x11,false)==ESP_OK);assert(count()==1);assert(disk[2].has_pending);
    assert(user_upgrade_finish(0x11,true)==ESP_OK);
    assert(count()==retain);assert(user_brightness_load()==(reset?70:23));assert(!disk[2].has_pending);
  }
  memset(disk,0,sizeof(disk));save();user_brightness_save(23);user_upgrade_stage(0x11,false,true);
  assert(user_upgrade_finish(0x10,true)==ESP_OK); // Rollback preserves both namespaces.
  assert(count()==1);assert(user_brightness_load()==23);assert(!disk[2].has_pending);
  user_upgrade_stage(0x11,false,true);fail_commit=1;assert(user_upgrade_finish(0x11,true)!=ESP_OK);
  assert(disk[2].has_pending);assert(user_upgrade_finish(0x11,true)==ESP_OK);assert(user_brightness_load()==70);
  assert(!disk[2].has_pending);assert(count()==0);
  for(int i=0;i<32;i++)save();
  cJSON *extra=cJSON_Parse("{\"name\":\"Full\",\"key_code\":\"001\"}");uint32_t unused=0;
  assert(user_signal_save(extra,&unused)!=ESP_OK);assert(count()==32);cJSON_Delete(extra);
  assert(user_brightness_save(0)==ESP_ERR_INVALID_ARG);
  puts("PASS: real storage CRUD, metadata/leading zeros, failures, limits, four OTA choices, pending boot, rollback and cleanup retry");
}
'''

mapping_checks = r'''
  memset(disk,0,sizeof(disk));
  for(unsigned f=0;f<3;f++) {
    cJSON *rec=cJSON_Parse(mapped_records[f]);assert(rec);
    assert(user_signal_button_code(rec)==mapped_expected[f]);
    uint32_t rid;assert(user_signal_save(rec,&rid)==ESP_OK);
    assert(user_signal_edit(rid,"Suffix","1")==ESP_ERR_INVALID_ARG);
    cJSON *original=cJSON_GetObjectItem(rec,"raw");
    const char *changes[]={"11","33","55","A7","00","FF"};
    const int expected[]={0x11,0x33,0x55,0xA7,0,255};
    for(unsigned n=0;n<6;n++) {
      assert(user_signal_edit(rid,"Suffix",changes[n])==ESP_OK);
      cJSON *list=NULL;assert(user_signals_read(&list)==ESP_OK);
      cJSON *edited=cJSON_GetArrayItem(list,f);
      assert(user_signal_button_code(edited)==expected[n]);
      cJSON *wave=cJSON_GetObjectItem(edited,"raw");
      int sum1=0,sum2=0;
      for(int j=0;j<82;j++) {
        int old=cJSON_GetArrayItem(original,j)->valueint, value=cJSON_GetArrayItem(wave,j)->valueint;
        if(j<66)assert(old==value);
        sum1+=old;sum2+=value;
      }
      assert(sum1==sum2);cJSON_Delete(list);
    }
    cJSON_Delete(rec);
  }
'''
fixtures=[json.loads((root/f'tests/fixtures/signal-{i}.json').read_text())['signals'][0] for i in (8,9,10)]
declaration='static const char *mapped_records[] = {'+','.join(json.dumps(json.dumps(r)) for r in fixtures)+'};\nstatic const int mapped_expected[]={0x55,0x11,0x33};\n'
source=source.replace('int main(void){',declaration+'int main(void){')
source=source.replace('  puts("PASS:',mapping_checks+'  puts("PASS:')

with tempfile.TemporaryDirectory(prefix="rf-user-data-") as directory:
    temporary = Path(directory)
    for name, content in headers.items():
        path = temporary / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content, encoding="utf-8")
    (temporary / "test.c").write_text(source, encoding="utf-8")
    binary = temporary / "check.exe"
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(temporary), "-I" + str(root / "components/user_data/include"),
                    "-I" + str(sdk / "components/json/cJSON"), "-I" + str(root / "components/rf_protocol/include"),
                    str(temporary / "test.c"), str(root / "components/user_data/user_data.c"),
                    str(sdk / "components/json/cJSON/cJSON.c"), str(root / "components/rf_protocol/rf_protocol.c"), "-o", str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)
