const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const handlers = {};
const classes = () => {
    const values = new Set();
    return { add: value => values.add(value), remove: value => values.delete(value), has: value => values.has(value) };
};
let submissions = 0;
let busy = false;
const form = { elements: { folder_id: { value: '' } }, requestSubmit() { submissions++; } };
const item = {
    dataset: { todoId: '7' }, classList: classes(), isConnected: true,
    closest(selector) { return selector === '.todo-item' ? this : null; },
    querySelector() { return form; },
};
const folder = {
    dataset: { folderId: '3' }, classList: classes(),
    closest() { return this; }, contains(other) { return other === this; },
    querySelector() { return details; },
};
const details = { open: true, closest() { return folder; } };
const root = { ...folder, dataset: { folderId: '0' }, classList: classes() };
const content = {
    contains() { return true; }, hasAttribute() { return busy; },
    querySelectorAll(selector) {
        return selector === '.folder-card' ? [folder] : [folder, root].filter(target => target.classList.has('is-drop-target'));
    },
};
const document = {
    getElementById() { return content; },
    addEventListener(type, handler) { handlers[type] = handler; },
};
vm.runInNewContext(fs.readFileSync(path.join(__dirname, '../frontend/folders.js'), 'utf8'), { document, Set });
function event(target) {
    return { target, dataTransfer: { setData(type, value) { this[type] = value; } },
        preventDefault() { this.prevented = true; } };
}
const start = event(item);
handlers.dragstart(start);
assert.equal(start.dataTransfer['text/plain'], '7');
assert.equal(start.dataTransfer.effectAllowed, 'move');
assert(item.classList.has('is-dragging'));
const over = event(folder);
handlers.dragover(over);
assert(over.prevented);
assert(folder.classList.has('is-drop-target'));
handlers.drop(event(folder));
assert.equal(form.elements.folder_id.value, '3');
assert.equal(submissions, 1);
handlers.dragend();
assert(!item.classList.has('is-dragging'));
assert(!folder.classList.has('is-drop-target'));
handlers.drop(event(folder));
assert.equal(submissions, 1, 'External drops cannot move todos');
handlers.dragstart(event(item));
handlers.drop(event(root));
assert.equal(form.elements.folder_id.value, '0');
assert.equal(submissions, 2);
handlers.dragend();
busy = true;
const blocked = event(item);
handlers.dragstart(blocked);
assert(blocked.prevented);
handlers.drop(event(folder));
assert.equal(submissions, 2);
busy = false;
handlers.toggle({ target: details });
details.open = false;
handlers.pagechange();
assert(details.open, 'Expanded folders survive soft navigation');
details.open = false;
handlers.toggle({ target: details });
handlers.pagechange();
assert(!details.open);
console.log('Folder drag/drop, ungrouping, busy state and expansion checks passed');
