"""Windows native LVGL and SeedFX tests using a portable Zig C/C++ compiler.

Generated objects, executables and framebuffer captures stay in .tmp/host-ui.
"""
from pathlib import Path
import concurrent.futures
import os
import subprocess
import sys
import json

ROOT=Path(__file__).resolve().parents[2]
OUT=ROOT/'.tmp/host-ui'
ZIG=Path(os.environ.get('SEEDFX_ZIG', ROOT/'.tmp/native-compiler/zig-x86_64-windows-0.14.1/zig.exe'))
LV=ROOT/'ESP32-P4-WIFI6-Touch-LCD-7B/managed_components/lvgl__lvgl'
DASH=ROOT/'ESP32-P4-WIFI6-Touch-LCD-7B/components/p4_audio_dashboard'

def main():
    OUT.mkdir(parents=True,exist_ok=True)
    os.environ['ZIG_GLOBAL_CACHE_DIR']=str(OUT/'zig-cache')
    os.environ['ZIG_LOCAL_CACHE_DIR']=str(OUT/'zig-local')
    includes=[ROOT/'tools/host/include',ROOT/'protocol',DASH/'include',DASH,LV,
              DASH.parent/'p4_wifi_settings/include']
    flags=['-target','x86_64-windows-gnu','-O1','-D_CRT_SECURE_NO_WARNINGS',
           '-DLV_CONF_SKIP','-DLV_USE_SNAPSHOT=1','-DLV_USE_STDLIB_MALLOC=1',
           '-DLV_FONT_MONTSERRAT_12=1','-DLV_FONT_MONTSERRAT_14=1',
           '-DLV_FONT_MONTSERRAT_16=1','-DLV_FONT_MONTSERRAT_20=1',
           '-DLV_FONT_MONTSERRAT_28=1','-DLV_FONT_MONTSERRAT_48=1']
    for p in includes: flags += ['-I',str(p)]
    sources=list((LV/'src').rglob('*.c'))+[DASH/'pedalboard_widgets.c',DASH/'pedal_patch.c',DASH/'pedal_layout.c',DASH/'pedal_grid.c',ROOT/'tools/host/test_pedalboard.c']
    header_freshness=max(p.stat().st_mtime for directory in
        (ROOT/'tools/host/include',ROOT/'protocol',DASH) for p in directory.rglob('*.h'))
    def compile_one(source):
        target=OUT/(str(source.relative_to(ROOT)).replace('\\','_').replace('/','_')+'.obj')
        freshness=max(source.stat().st_mtime,Path(__file__).stat().st_mtime,
            max((DASH/'audio_dashboard.c').stat().st_mtime,(DASH/'wifi_view.c').stat().st_mtime) if source.name=='test_pedalboard.c' else 0,
            header_freshness if source.name in ('test_pedalboard.c','pedalboard_widgets.c','pedal_patch.c','pedal_layout.c','pedal_grid.c') else 0)
        if not target.exists() or target.stat().st_mtime<freshness:
            result=subprocess.run([str(ZIG),'cc',*flags,'-c',str(source),'-o',str(target)],capture_output=True,text=True)
            if result.returncode: raise RuntimeError(result.stdout+result.stderr)
        return target
    with concurrent.futures.ThreadPoolExecutor(max_workers=10) as pool:
        objects=list(pool.map(compile_one,sources))
    exe=OUT/'pedalboard-test.exe'
    response=OUT/'link.rsp'
    response.write_text('\n'.join(json.dumps(p.as_posix()) for p in objects),encoding='utf-8')
    subprocess.run([str(ZIG),'cc','-target','x86_64-windows-gnu','@'+str(response),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],cwd=OUT,check=True)
    print('Native LVGL captures:',OUT)
    cases=[]
    for path in sorted((ROOT/'microSD-ready/SEEDFX/effects').rglob('*.json')):
        fx=json.loads(path.read_text(encoding='utf-8'))
        values=[str(float(p['default']))+'f' for p in fx['parameters']]
        values+=['0.f']*(4-len(values))
        cases.append('{%d,%d,{%s}}' % (fx['effect_type'],len(fx['parameters']),','.join(values)))
    (OUT/'catalog_cases.h').write_text('static const struct {unsigned short type; unsigned count; float params[4];} cases[]={'+','.join(cases)+'};',encoding='ascii')
    dsp=OUT/'seedfx-test.exe'
    subprocess.run([str(ZIG),'c++','-target','x86_64-windows-gnu','-std=c++14','-O1',
        '-I'+str(ROOT/'tools/host/include'),'-I'+str(ROOT/'protocol'),'-I'+str(OUT),
        str(ROOT/'tools/host/test_seedfx.cpp'),str(ROOT/'Seed3/src/seedfx_graph.cpp'),'-o',str(dsp)],check=True)
    subprocess.run([str(dsp)],cwd=OUT,check=True)
    meter=OUT/'meter-test.exe'
    subprocess.run([str(ZIG),'c++','-target','x86_64-windows-gnu','-std=c++14','-O1',
        '-I'+str(ROOT/'tools/host/include'),'-I'+str(ROOT/'protocol'),
        '-I'+str(ROOT/'Seed3/src'),str(ROOT/'tools/host/test_meter.cpp'),
        str(ROOT/'Seed3/src/seed_level_meter.cpp'),'-o',str(meter)],check=True)
    subprocess.run([str(meter)],cwd=OUT,check=True)

if __name__=='__main__': main()
