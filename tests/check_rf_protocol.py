"""Host checks of the exact fixed-code codec used by the firmware."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rf_protocol.h"
static rf_frame_t original, received;
int main(void) {
    const uint32_t values[]={0,1,0x123456,0xffffff,0xffffffff};
    for(unsigned n=0;n<5;n++) {
        memset(&original,0,sizeof(original)); original.frequency_hz=433920000;
        unsigned bits=n==4?32:24;
        assert(rf_frame_encode(&original,values[n],bits,350));
        received=original;
        memcpy(received.duration+original.count,original.duration,original.count*sizeof(uint16_t));
        received.count*=2;
        // Deterministic +/-10% timing jitter across both repeated frames.
        for(unsigned i=0;i<received.count;i++) received.duration[i]=received.duration[i]*(i%2?110:90)/100;
        assert(rf_frame_analyze(&received));
        assert(received.bits==bits&&received.code==values[n]&&received.repeats==2);
        assert(rf_frame_same(&original,&received));
    }
    uint32_t code;
    assert(rf_code_parse("000001",24,&code)&&code==1);
    assert(!rf_code_parse("1000000",24,&code));assert(!rf_code_parse("X12",24,&code));
    assert(!rf_code_parse("",24,&code));assert(!rf_frame_encode(&original,0x100,8,350));
    assert(!rf_frame_encode(&original,0,24,99));
    rf_frame_encode(&original,0xabcdef,24,350);
    received=original;received.duration[10]=40;
    assert(rf_frame_analyze(&received)&&received.bits==0); // Never decode suffix after an invalid pulse.
    received=original;received.count=12;assert(!rf_frame_analyze(&received));
    received=original;received.count=RF_MAX_PULSES+1;assert(!rf_frame_analyze(&received));
    received=original;received.duration[received.count-1]=15000;received.tail_clipped=true;
    assert(rf_frame_analyze(&received)&&received.bits==0);
    received=original;received.duration[received.count-1]=350*20;
    assert(rf_frame_analyze(&received)&&received.bits==0);
    received=original;received.duration[received.count-1]=350*40;
    assert(rf_frame_analyze(&received)&&received.bits==0);
    // Captures may begin halfway through a burst. Prefer a later complete repeated frame.
    rf_frame_encode(&original,0x123456,24,350);
    received=original;
    memmove(received.duration,original.duration+8,(original.count-8)*sizeof(uint16_t));
    unsigned partial=original.count-8;
    memcpy(received.duration+partial,original.duration,original.count*sizeof(uint16_t));
    memcpy(received.duration+partial+original.count,original.duration,original.count*sizeof(uint16_t));
    received.count=partial+original.count*2;
    assert(rf_frame_analyze(&received)&&received.bits==24&&received.code==0x123456&&received.repeats==2);
    received=original;received.first_level=0;
    memmove(received.duration+1,received.duration,received.count*sizeof(uint16_t));
    received.duration[0]=2000;received.count++;
    assert(rf_frame_analyze(&received)&&received.code==0x123456&&received.bits==24);
    rf_frame_encode(&original,0x123456,24,500);
    received=original;
    // Split a 1500 us high into 700 high, 30 low glitch, 770 high.
    unsigned spike=0;
    while(original.duration[spike]!=1500) spike++;
    memmove(received.duration+spike+3,received.duration+spike+1,
            (received.count-spike-1)*sizeof(uint16_t));
    received.duration[spike]=700;received.duration[spike+1]=30;received.duration[spike+2]=770;
    received.count+=2;
    assert(rf_frame_analyze(&received) && received.bits==24 && received.code==0x123456);
    puts("PASS: actual PWM encode/decode, zeroes, 24/32 bits, repeats/jitter, corruption, partial frames, bounds and code edits");
}
'''
compiler = root / "simulator/.tools/w64devkit/bin/gcc.exe"
env = dict(os.environ, PATH=str(compiler.parent) + os.pathsep + os.environ["PATH"])
with tempfile.TemporaryDirectory(prefix="rf-codec-") as directory:
    test = Path(directory) / "test.c"
    test.write_text(source, encoding="utf-8")
    binary = Path(directory) / "check.exe"
    subprocess.run([str(compiler), "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(root / "components/rf_protocol/include"), str(test),
                    str(root / "components/rf_protocol/rf_protocol.c"), "-o", str(binary)], check=True, env=env)
    subprocess.run([str(binary)], check=True, env=env)
