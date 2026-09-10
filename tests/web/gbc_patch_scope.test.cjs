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
