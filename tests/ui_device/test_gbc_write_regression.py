"""Authorized MBC3 board write matrix. Leaves the final requested ROM in the cart."""
import argparse, hashlib, json, subprocess, sys, time, urllib.parse, urllib.request
from pathlib import Path

def main():
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--url',required=True);p.add_argument('--port',required=True)
    p.add_argument('--rom-a',required=True);p.add_argument('--rom-b',required=True)
    p.add_argument('--out',required=True);a=p.parse_args()
    out=Path(a.out);out.mkdir(parents=True,exist_ok=True)
    root=Path(__file__).resolve().parents[2]
    def api(path,params=None,method='GET'):
        if params: path += '?' + urllib.parse.urlencode(params)
        with urllib.request.urlopen(urllib.request.Request(a.url+path,method=method),timeout=60) as r:
            return json.load(r)
    def download(path):
        with urllib.request.urlopen(a.url+'/api/tf/download?'+urllib.parse.urlencode({'path':path}),timeout=60) as r:
            return r.read()
    hashes={rom:hashlib.sha256(download(rom)).hexdigest() for rom in (a.rom_a,a.rom_b)}
    rows=[]
    for index,(link,write,erase,rom) in enumerate([
        ('cpld','psram','smart',a.rom_a),('legacy','direct','force',a.rom_b),
        ('cpld','pipeline','smart',a.rom_a)]):
        result=out/f'{index}_{link}_{write}.jsonl'
        subprocess.run([sys.executable,str(root/'tools/ag32_benchmark.py'),'--url',a.url,'--port',a.port,
            '--action','write','--mode','mbc5','--link',link,'--write-path',write,'--erase-mode',erase,
            '--rom',rom,'--results',str(result),'--label','v119-mbc3-regression'],check=True)
        r=json.loads(result.read_text(encoding='utf-8').splitlines()[-1]);assert r['ok'],r
        assert api('/api/music/status')['state'] not in ('playing','loading')
        row={'rom':rom,'rom_sha256':hashes[rom],'link':link,'path':write,'erase':erase,
            'task_ms':r['job']['status']['task_time_ms'],'write_ms':r['job']['status']['write_time_ms'],
            'erase_ms':r['job']['status']['erase_time_ms'],'verified':r['verify']['status']['processed']}
        dump=api('/api/read',{'mode':'mbc5','size':'2MB','name':f'diag_v119_{link}_{write}.gbc'},'POST')
        assert dump['ok'],dump
        end=time.monotonic()+30
        while time.monotonic()<end:
            s=api('/api/status')
            if s['state'] in ('done','error'): break
            time.sleep(.2)
        assert s['state']=='done' and s['message']=='dump finished',s
        rel=dump['path'].removeprefix('/sdcard/')
        data=download(rel);row['dump_sha256']=hashlib.sha256(data).hexdigest()
        assert len(data)==2097152 and row['dump_sha256']==row['rom_sha256'],row
        row['dump_ms']=s['task_time_ms'];rows.append(row)
        print('CHECKED '+json.dumps(row,ensure_ascii=False),flush=True)
        (out/'matrix.json').write_text(json.dumps(rows,ensure_ascii=False,indent=2),encoding='utf-8')
    api('/api/burn/core_config',{'ag32_link':'auto'},'POST')
    print('All writes, independent verifies and downloaded dumps matched.',flush=True)
if __name__=='__main__':main()
