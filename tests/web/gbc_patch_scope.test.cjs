const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const html = fs.readFileSync('.web/main.html', 'utf8');
const elements = new Map();
const document = {getElementById(id) {
    if (!elements.has(id)) elements.set(id, {style: {}, classList: {add() {}}});
    return elements.get(id);
}};
const context = vm.createContext({document, state: {}, formatSize: String,
    getFileIcon: () => '', getEffectiveWritePath: () => 'psram', getPipelineEraseMode: () => 'smart',
    getPsramWindowMb: () => 4, updateWritePathControls() {}});
const start = html.indexOf('function showFileActionModal(');
const end = html.indexOf('function closeFileActionModal(', start);
vm.runInContext(html.slice(start, end), context);
for (const [name, display] of [['game.gba', 'flex'], ['game.gbc', 'none'], ['game.gb', 'none'], ['game.GBA', 'flex']]) {
    context.showFileActionModal(name, 1024, name);
    assert.equal(document.getElementById('fileActionWaitcntSection').style.display, display);
}
console.log('GBA-only patch controls are hidden for GB/GBC files');

const voltageStart = html.indexOf('async function refreshGbcVoltage(');
const voltageEnd = html.indexOf('function setCartMode(', voltageStart);
const calls = [];
let voltageResponse = {ok: true, gbc_voltage: '3v3'};
context.apiCall = async (...args) => { calls.push(args); return {ok: voltageResponse.ok, status: 409}; };
context.readApiPayload = async () => voltageResponse;
context.showToast = () => {};
vm.runInContext(html.slice(voltageStart, voltageEnd), context);
(async () => {
    await context.refreshGbcVoltage();
    const select = document.getElementById('burnGbcVoltage');
    assert.equal(select.value, '3v3');
    voltageResponse = {ok: true, gbc_voltage: '5v'};
    await context.setGbcVoltage('5v');
    assert.equal(calls.at(-1)[0], '/api/burn/core_config?gbc_voltage=5v');
    assert.equal(calls.at(-1)[1].method, 'POST');
    assert.equal(select.value, '5v');
    voltageResponse = {ok: false, message: 'burn task is running'};
    await context.setGbcVoltage('3v3');
    assert.equal(select.value, '5v');
    assert.equal(select.disabled, false);
    assert.equal((html.match(/id="burnGbcVoltage"/g) || []).length, 1);
    const settings = html.slice(html.indexOf('function renderSettingsPage('), html.indexOf('function renderSettingsPage(') + 14000);
    assert(!settings.includes('id="burnGbcVoltage"'));
    console.log('GBC voltage: load, isolated save and rejected change passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
