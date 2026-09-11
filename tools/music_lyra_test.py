"""Quiet real-board format/control regression; requires pyserial and FFmpeg.
Generates attenuated fixtures, plays at 1%, removes only its own TF fixtures.
"""
import argparse,json,time,subprocess,urllib.request,urllib.parse
from pathlib import Path
import serial

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--url',required=True);p.add_argument('--port',required=True)
    p.add_argument('--out',required=True);p.add_argument('--ffmpeg',default='C:/ffmpeg/bin/ffmpeg.exe')
    p.add_argument('--only',help='Comma-separated fixture names to run from the matrix')
    args=p.parse_args();out=Path(args.out);out.mkdir(parents=True,exist_ok=True)
    def api(path,body=None,method=None):
        data=None if body is None else json.dumps(body).encode()
        with urllib.request.urlopen(urllib.request.Request(args.url+path,data=data,
             headers={'Content-Type':'application/json'},method=method),timeout=20) as r:
            result=json.load(r)
        assert result.get('ok',True),result
        return result
    formats=[('cbr.mp3',44100,2,'libmp3lame',['-b:a','128k']),
             ('vbr.mp3',48000,2,'libmp3lame',['-q:a','2']),
             ('mono.mp3',22050,1,'libmp3lame',['-b:a','32k']),
             ('16.flac',44100,2,'flac',['-sample_fmt','s16']),
             ('24.flac',48000,2,'flac',['-sample_fmt','s32']),
             ('pcm.wav',44100,2,'pcm_s16le',[]),
             ('aac.m4a',44100,2,'aac',[]),
             ('vorbis.ogg',44100,2,'libvorbis',[]),
             ('opus.opus',48000,2,'libopus',[]),
             ('pcm.aiff',44100,2,'pcm_s16be',[]),
             ('low.wav',8000,1,'pcm_u8',[]),
             ('96.flac',96000,2,'flac',['-sample_fmt','s32']),
             ('alac.m4a',44100,2,'alac',['-movflags','+faststart'])]
    fixtures=[]
    for name,rate,ch,codec,flags in formats:
        if args.only and name not in args.only.split(','): continue
        file=out/('diag_lyra_'+name)
        signal='anoisesrc=amplitude=0.1:duration=8:seed=116' if name=='96.flac' else 'sine=frequency=440:duration=8'
        subprocess.run([args.ffmpeg,'-v','error','-y','-f','lavfi','-i',signal,
            '-af','volume=0.001','-ar',str(rate),'-ac',str(ch),'-c:a',codec,*flags,str(file)],check=True)
        fixtures.append(file)
    port=serial.Serial();port.port=args.port;port.baudrate=115200;port.timeout=0.1;port.dtr=port.rts=False
    log=(out/'serial.log').open('w',encoding='utf-8');results=[];uploaded=[]
    def drain():
        while port.in_waiting:
            line=port.readline().decode('utf-8','replace');log.write(line);log.flush()
            assert 'Guru Meditation' not in line and 'Stack canary' not in line,line
    def wait_state(states,timeout=15):
        start=time.monotonic()
        while time.monotonic()-start<timeout:
            drain();s=api('/api/music/status')
            assert s['volume']==1,s
            if s['state'] in states:return s
            assert s['state']!='error',s
            time.sleep(0.1)
        raise TimeoutError(s)
    try:
        api('/api/music/stop',{},'POST');api('/api/music/volume',{'volume':1},'POST')
        port.open()
        for file in fixtures:
            with urllib.request.urlopen(urllib.request.Request(args.url+'/api/tf/upload?dir=&name='+file.name,
                 data=file.read_bytes(),method='POST'),timeout=30) as r: assert json.load(r)['ok']
            uploaded.append(file.name)
            port.write(('play /sdcard/'+file.name+'\n').encode())
            started=wait_state({'playing'})
            api('/api/music/pause',{},'POST');paused=wait_state({'paused'})
            api('/api/music/seek',{'delta':len(file.read_bytes())//3},'POST')
            seeked=wait_state({'paused'})
            api('/api/music/pause',{},'POST');wait_state({'playing','finished'})
            finished=wait_state({'finished'},20)
            api('/api/music/stop',{},'POST');wait_state({'idle'})
            results.append({'file':file.name,'started':started,'paused':paused,'seeked':seeked,'finished':finished})
            print(json.dumps({'ok':True,'file':file.name,'rate':finished['sample_rate'],'bits':finished['bits_per_sample']},ensure_ascii=False),flush=True)
    finally:
        try: api('/api/music/stop',{},'POST');time.sleep(0.2);drain()
        finally: port.close();log.close()
        (out/'results.json').write_text(json.dumps(results,ensure_ascii=False,indent=2),encoding='utf-8')
        for name in uploaded:
            print('Removed TF fixture:',name,api('/api/tf/delete?path='+urllib.parse.quote(name),method='DELETE'),flush=True)

if __name__=='__main__':main()
