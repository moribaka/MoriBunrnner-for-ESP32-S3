const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync('.web/main.html', 'utf8');
for (const match of html.matchAll(/<script(?:\s[^>]*)?>([\s\S]*?)<\/script>/g)) {
    if (match[1].trim()) new vm.Script(match[1]);
}
const start = html.indexOf('const pendingStatusRequests =');
const end = html.indexOf('function refreshStatus()', start);
assert(start >= 0 && end > start);
let fetchCount = 0;
let release;
const context = vm.createContext({
    fetch: () => { fetchCount++; return new Promise(resolve => { release = resolve; }); },
});
vm.runInContext(html.slice(start, end), context);
(async () => {
    const first = context.fetchSharedStatus('/api/status');
    const second = context.fetchSharedStatus('/api/status');
    assert.equal(fetchCount, 1);
    release(new Response('{"state":"done"}'));
    assert.equal((await (await first).json()).state, 'done');
    assert.equal((await (await second).json()).state, 'done');
    const third = context.fetchSharedStatus('/api/status');
    assert.equal(fetchCount, 2);
    release(new Response('{"state":"idle"}'));
    assert.equal((await (await third).json()).state, 'idle');

    let runs = 0;
    let finish;
    const work = () => { runs++; return new Promise(resolve => { finish = resolve; }); };
    const one = context.runStatusRefresh('burn', work);
    const two = context.runStatusRefresh('burn', work);
    assert.equal(one, two);
    await Promise.resolve();
    assert.equal(runs, 1);
    finish(42);
    assert.equal(await one, 42);
    await assert.rejects(context.runStatusRefresh('burn', () => { throw new Error('offline'); }));
    assert.equal(await context.runStatusRefresh('burn', () => 7), 7);
    console.log('Web syntax, shared response bodies, overlap guard and retry passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
