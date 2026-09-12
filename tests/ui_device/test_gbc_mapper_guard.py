"""MBC3 board: wrong mapper and oversized ROM must be rejected before erase.

Requires the named good ROM to already match the cart. Never deliberately
issues these negative write requests against firmware older than v2.32.119.
"""
import argparse, json, sys, time, urllib.request, urllib.parse, urllib.error
from test_file_worker import Board

def api(base, path, params=None, method='GET'):
    if params: path += '?' + urllib.parse.urlencode(params)
    try:
        with urllib.request.urlopen(urllib.request.Request(base+path, method=method), timeout=20) as r:
            return r.status, json.load(r)
    except urllib.error.HTTPError as e:
        return e.code, {'ok':False, 'message':e.read().decode('utf-8')}

def select_rom(b, rom, wrong=False):
    state = b.ui()
    if state['screen_dimmed']: state = b.key('b')
    for _ in range(8):
        if state['page'] == 0: break
        state = b.key('b')
    assert state['page'] == 0, state
    b.select(0, lambda s: '烧录' in s['selection'] or 'Burner' in s['selection'], 'right')
    assert b.key('a')['page'] == 14
    b.select(14, lambda s: s['selected'] == 1, 'right')
    assert b.key('a')['page'] == 15
    b.select(15, lambda s: s['selected'] == 0)
    b.key('a')
    end = time.monotonic()+20
    while time.monotonic() < end:
        state = b.ui()
        if state['count'] == 12: break
        time.sleep(.1)
    assert state['count'] == 12, state
    b.select(15, lambda s: s['selected'] == 2)
    assert b.key('a')['page'] == 6
    for part in rom.split('/'):
        b.select(6, lambda s: s['selection'] == part)
        state = b.key('a')
    assert state['page'] == 15 and state['count'] == 2, state
    assert state['selected'] == 1 and 'MBC3' in state['selection'], state
    if wrong: b.select(15, lambda s: s['selected'] == 0)
    b.key('a')

def main():
    sys.stdout.reconfigure(encoding='utf-8')
    p=argparse.ArgumentParser();p.add_argument('--url',required=True);p.add_argument('--port',required=True)
    p.add_argument('--good-rom',required=True);p.add_argument('--large-rom',required=True)
    p.add_argument('--log',required=True);a=p.parse_args()
    b=Board(a.port,a.log)
    try:
        assert b.cmd('status')['version']=='v2.32.119'
        select_rom(b,a.good_rom,wrong=True)
        code, wrong = api(a.url,'/api/write',{'mode':'mbc5','name':a.good_rom},'POST')
        assert code>=400 and not wrong.get('ok',True),wrong
        select_rom(b,a.large_rom)
        code, large = api(a.url,'/api/write',{'mode':'mbc5','name':a.large_rom},'POST')
        assert code>=400 and not large.get('ok',True),large
        assert '2097152' in json.dumps(large),large
        code, accepted=api(a.url,'/api/verify',{'mode':'mbc5','name':a.good_rom},'POST')
        assert code==200,accepted
        end=time.monotonic()+30
        while time.monotonic()<end:
            b.ui();_,s=api(a.url,'/api/status')
            if s['state'] in ('done','error'): break
            time.sleep(.1)
        assert s['state']=='done' and s['message']=='verify finished',s
        print(json.dumps({'ok':True,'wrong_mapper':wrong,'oversize':large,
                          'unchanged_bytes':s['processed']},ensure_ascii=False),flush=True)
    finally: b.serial.close();b.log.close()
if __name__=='__main__': main()
