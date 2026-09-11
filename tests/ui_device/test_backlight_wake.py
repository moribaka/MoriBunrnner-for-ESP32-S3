"""Idle screen + volume-save + brightness adjustment + wake regression."""
import argparse,json,time,urllib.request
from test_file_worker import Board

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--url',required=True);p.add_argument('--port',required=True)
    p.add_argument('--log',required=True)
    args=p.parse_args()
    def api(path,body=None):
        data=None if body is None else json.dumps(body).encode()
        with urllib.request.urlopen(urllib.request.Request(args.url+path,data=data,
             headers={'Content-Type':'application/json'}),timeout=10) as r:return json.load(r)
    def saved_brightness():
        with urllib.request.urlopen(args.url+'/api/tf/download?path=.setting%2Fmori_system.ini',timeout=10) as r:
            text=r.read().decode('utf-8')
        return next(int(line.split('=',1)[1]) for line in text.splitlines() if line.startswith('brightness='))
    b=Board(args.port,args.log);b.serial.timeout=1
    try:
        api('/api/music/stop',{});api('/api/music/volume',{'volume':1})
        api('/api/device/brightness',{'brightness':128})
        s=b.ui();b.key('b')
        start=time.monotonic()
        while time.monotonic()-start<90:
            s=b.ui()
            if s['screen_dimmed']:break
            time.sleep(1)
        assert s['screen_dimmed'],s
        dark=s
        assert api('/api/device/brightness')['brightness']==128
        api('/api/music/volume',{'volume':1})
        assert saved_brightness()==128
        assert b.ui()['screen_dimmed']
        api('/api/device/brightness',{'brightness':77})
        assert b.ui()['screen_dimmed']
        assert saved_brightness()==77
        wake=b.key('b');assert not wake['screen_dimmed'],wake
        assert api('/api/device/brightness')['brightness']==77
        assert b.ui()['process_calls']>wake['process_calls']
        api('/api/device/brightness',{'brightness':128})
        print(json.dumps({'ok':True,'dark':dark,'wake':wake,'saved':saved_brightness(),
            'music':api('/api/music/status'),'status':b.cmd('status')},ensure_ascii=False),flush=True)
    finally:
        b.serial.close();b.log.close()

if __name__=='__main__':main()
