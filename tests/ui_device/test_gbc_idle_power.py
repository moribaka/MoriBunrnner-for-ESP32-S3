"""Read-only MBC3 verification after cartridge auto-power-off; records PMIC samples."""
import argparse,json,sys,time,threading,urllib.request,urllib.parse
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'tools'))
from ag32_benchmark import capture_serial
def main():
    p=argparse.ArgumentParser();p.add_argument('--url',required=True);p.add_argument('--port',required=True)
    p.add_argument('--rom',required=True);p.add_argument('--out',required=True);p.add_argument('--cycles',type=int,default=3)
    a=p.parse_args();sys.stdout.reconfigure(encoding='utf-8');out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
    stop=threading.Event();t=threading.Thread(target=capture_serial,args=(a.port,out/'serial.log',stop,[]));t.start()
    def api(path,params=None,method='GET'):
        if params:path+='?'+urllib.parse.urlencode(params)
        with urllib.request.urlopen(urllib.request.Request(a.url+path,method=method),timeout=5) as r:return json.load(r)
    result={'ok':False,'cycles':[]}
    try:
        with (out/'power.jsonl').open('a',encoding='utf-8') as log:
            def sample(phase):
                s=api('/api/status');power=api('/api/power/status')
                log.write(json.dumps({'phase':phase,'wall':time.time(),'status':s,'power':power})+'\n');log.flush()
                return s,power
            for i in range(a.cycles):
                if i:
                    end=time.monotonic()+75
                    while time.monotonic()<end:
                        s,power=sample('idle')
                        if s['cart_sleeping']:break
                        time.sleep(1)
                    assert s['cart_sleeping'],s
                s,before=sample('before')
                accepted=api('/api/verify',{'mode':'mbc5','name':a.rom},'POST');assert accepted['ok'],accepted
                end=time.monotonic()+30
                while time.monotonic()<end:
                    s,power=sample('verify')
                    if s['state'] in ('done','error'):break
                    time.sleep(.1)
                assert s['state']=='done' and s['message']=='verify finished',s
                result['cycles'].append({'cycle':i,'bytes':s['processed'],'task_ms':s['task_time_ms'],'before':before,'after':power})
                print(json.dumps(result['cycles'][-1],ensure_ascii=False),flush=True)
            result['ok']=True
    except Exception as e:
        result['error']=str(e);raise
    finally:
        stop.set();t.join();(out/'result.json').write_text(json.dumps(result,ensure_ascii=False,indent=2),encoding='utf-8')
if __name__=='__main__':main()
