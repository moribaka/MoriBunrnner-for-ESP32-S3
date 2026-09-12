"""Silent audio priority test; --write-rom explicitly opts into a cartridge write."""
import argparse,json,time,subprocess,urllib.request,urllib.parse
from pathlib import Path
from test_file_worker import Board

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--url',required=True);p.add_argument('--port',required=True)
    p.add_argument('--mode',choices=['gba','mbc5'],required=True)
    p.add_argument('--size',default='32MB',help='Must fit the inserted mapper/cartridge')
    p.add_argument('--write-rom',help='Authorized TF ROM to burn instead of the read-only dump')
    p.add_argument('--out',required=True)
    a=p.parse_args();out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
    name='diag_priority_silence.mp3';f=out/name
    subprocess.run(['C:/ffmpeg/bin/ffmpeg.exe','-v','error','-y','-f','lavfi','-i',
        'anullsrc=r=44100:cl=stereo','-t','60','-c:a','libmp3lame','-b:a','32k',str(f)],check=True)
    def api(path,body=None,method=None):
        data=None if body is None else json.dumps(body).encode()
        with urllib.request.urlopen(urllib.request.Request(a.url+path,data=data,
             headers={'Content-Type':'application/json'},method=method),timeout=30) as r:return json.load(r)
    def wait(predicate,timeout=20):
        end=time.monotonic()+timeout
        while time.monotonic()<end:
            b.ui();s=api('/api/music/status')
            if predicate(s):return s
            time.sleep(.15)
        raise AssertionError(s)
    b=Board(a.port,str(out/'serial.log'));b.serial.timeout=1;uploaded=False;dump=None;result={}
    try:
        assert api('/api/music/volume',{'volume':1})['ok']
        with urllib.request.urlopen(urllib.request.Request(a.url+'/api/tf/upload?dir=&name='+name,
             data=f.read_bytes(),method='POST'),timeout=30) as r:assert json.load(r)['ok']
        uploaded=True
        b.cmd('play /sdcard/'+name,'play')
        before=wait(lambda s:s['state']=='playing' and s['position']>0)
        memory_before=b.cmd('status')
        if a.write_rom:
            accepted=api('/api/write?'+urllib.parse.urlencode({'mode':a.mode,'name':a.write_rom,
                'write_path':'psram','pipeline_erase':'smart','sram':0,'waitcnt':0,'batteryless':0}),{},'POST')
        else:
            accepted=api('/api/read?mode='+a.mode+'&size='+a.size+'&name=diag_priority_read',{},'POST')
            dump=accepted['path']
        assert accepted['ok'],accepted
        paused=wait(lambda s:s['state']=='paused')
        assert api('/api/status')['state']=='burning'
        assert not api('/api/music/pause',{})['ok']
        assert not api('/api/music/seek',{'delta':1000})['ok']
        b.serial.write(('play /sdcard/'+name+'\n').encode())
        # Wait for this command's terminal rejection; USB output can arrive
        # after the UART buffer initially becomes empty.
        end=time.monotonic()+5
        rejected=False
        while time.monotonic()<end:
            line=b.line()
            if '@mori ' not in line:continue
            event=json.loads(line.split('@mori ',1)[1])
            if event.get('event')=='error':
                assert event.get('message')=='ESP_ERR_INVALID_STATE',event
                rejected=True;break
            assert event.get('event')!='play',event
        assert rejected,'Missing playback rejection'
        memory_during=b.cmd('status')
        end=time.monotonic()+90
        while time.monotonic()<end:
            s=api('/api/status');music=api('/api/music/status');b.ui()
            assert music['state']=='paused' and music['position']==paused['position'],music
            if s['state'] in ('done','error','cancelled'):break
            time.sleep(.4)
        assert s['state']=='done' and s['message']==('burn finished' if a.write_rom else 'dump finished'),s
        assert api('/api/music/status')['state']=='paused'
        assert api('/api/music/pause',{})['ok']
        resumed=wait(lambda s:s['state']=='playing')
        assert resumed['volume']==1
        result={'ok':True,'before':before,'paused':paused,'resumed':resumed,
                'memory_before':memory_before,'memory_during':memory_during,
                'write' if a.write_rom else 'read':s}
        print(json.dumps(result,ensure_ascii=False),flush=True)
    finally:
        api('/api/music/stop',{})
        active=api('/api/status')['state']=='burning'
        if active:
            api('/api/cancel',{},'POST')
            end=time.monotonic()+30
            while api('/api/status')['state']=='burning' and time.monotonic()<end:time.sleep(.2)
        b.serial.close();b.log.close()
        (out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
        if uploaded:api('/api/tf/delete?path='+name,method='DELETE')
        if dump:
            # Cancellation removes the unfinished dump itself.
            listing=api('/api/tf/list?dir=ROM_OUTPUT')
            rel=dump.removeprefix('/sdcard/')
            if any(entry.get('path')==rel for entry in listing.get('entries',[])):
                api('/api/tf/delete?path='+urllib.parse.quote(rel),method='DELETE')

if __name__=='__main__':main()
