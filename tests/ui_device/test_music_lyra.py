"""Native music library and pause/resume integration at 1% volume."""
import argparse,json,time,urllib.request
from test_file_worker import Board

def main():
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--url',required=True);p.add_argument('--port',required=True)
    p.add_argument('--rom',required=True);p.add_argument('--log',required=True)
    a=p.parse_args()
    def api(path,body=None):
        data=None if body is None else json.dumps(body).encode()
        with urllib.request.urlopen(urllib.request.Request(a.url+path,data=data,
             headers={'Content-Type':'application/json'}),timeout=15) as r:return json.load(r)
    b=Board(a.port,a.log)
    b.serial.timeout=1
    try:
        assert api('/api/music/volume',{'volume':1})['ok']
        b.cmd('play /sdcard/'+a.rom.removeprefix('/sdcard/'),'play')
        deadline=time.monotonic()+20
        while time.monotonic()<deadline:
            s=api('/api/music/status')
            if s['state']=='playing':break
            assert s['state']!='error',s
            time.sleep(.2)
        assert s['state']=='playing' and s['volume']==1,s
        assert s['path']==a.rom.removeprefix('/sdcard/'),s
        state=b.ui()
        if state['screen_dimmed']:state=b.key('b')
        for _ in range(8):
            if state['page']==0:break
            state=b.key('b')
        b.select(0,lambda s:'音乐' in s['selection'] or 'Music' in s['selection'],'right')
        player=b.key('a');time.sleep(.5)
        assert '失败' not in player['status'] and 'failed' not in player['status'],player
        # Music player is a canvas page: the generic UI row count is always 0.
        drawer=b.key('menu');assert drawer['page']==player['page'],drawer
        b.key('menu')
        b.key('a');time.sleep(.5)
        paused=api('/api/music/status');assert paused['state']=='paused',paused
        b.key('a');time.sleep(.5)
        resumed=api('/api/music/status');assert resumed['state']=='playing' and resumed['volume']==1,resumed
        api('/api/music/stop',{});time.sleep(.3)
        final=b.key('b');assert final['page']==0,final
        print(json.dumps({'ok':True,'player':player,'drawer':drawer,'paused':paused,
            'resumed':resumed,'final':final,'memory':b.cmd('status')},ensure_ascii=False),flush=True)
    finally:
        api('/api/music/stop',{});b.serial.close();b.log.close()

if __name__=='__main__':main()
