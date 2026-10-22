"""Decode all three measured remotes at every DMA starting phase."""
from pathlib import Path
import json, os, subprocess, tempfile
root=Path(__file__).resolve().parents[1]
source='#include <assert.h>\n#include <stdio.h>\n#include <string.h>\n#include "rf_protocol.h"\n'
for i in (8,9,10):
    data=json.loads((root/f'tests/fixtures/signal-{i}.json').read_text())['signals'][0]
    source+=f'static const uint16_t wave{i}[]={{'+','.join(map(str,data['raw']))+'};\n'
source+=r'''
int main(void) {
    const uint16_t *waves[]={wave8,wave9,wave10};
    const char *expected[]={"2D789C0555","2D789C0511","2D789C0533"};
    for(unsigned n=0;n<3;n++) {
        for(unsigned phase=0;phase<82;phase++) {
            rf_frame_t f={.count=256,.first_level=1^(phase&1),.tail_clipped=true};
            for(unsigned i=0;i<256;i++)f.duration[i]=waves[n][(phase+i)%82];
            assert(rf_frame_analyze(&f));
            assert(!strcmp(f.raw_code,expected[n]) && f.repeats>=2);
            assert(f.count==82 && f.first_level==1 && !f.tail_clipped && f.bits==0);
            assert(!memcmp(f.duration,waves[n],82*sizeof(uint16_t)));
        }
        rf_frame_t f={.count=82,.first_level=1};memcpy(f.duration,waves[n],sizeof(wave8));
        f.duration[20]=2000;assert(rf_frame_analyze(&f));assert(!f.raw_code[0]);
    }
    puts("PASS: 3 real 40-bit keys across 82 DMA phases, waveform preservation, corrupt-data rejection");
}
'''
cc=root/'simulator/.tools/w64devkit/bin/gcc.exe'
env=dict(os.environ,PATH=str(cc.parent)+os.pathsep+os.environ['PATH'])
with tempfile.TemporaryDirectory() as d:
    test=Path(d)/'test.c';exe=Path(d)/'test.exe';test.write_text(source)
    subprocess.run([str(cc),'-std=c11','-Wall','-Wextra','-Werror','-I'+str(root/'components/rf_protocol/include'),str(test),str(root/'components/rf_protocol/rf_protocol.c'),'-o',str(exe)],check=True,env=env)
    subprocess.run([str(exe)],check=True,env=env)
