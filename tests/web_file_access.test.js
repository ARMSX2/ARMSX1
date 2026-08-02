'use strict';

const fs = require('fs');
const path = require('path');
const vm = require('vm');

const root = path.resolve(__dirname, '..');
const context = {
  console,
  window: {},
  Module: {},
};
vm.createContext(context);
vm.runInContext(fs.readFileSync(path.join(root, 'web', 'file_access.js'), 'utf8'), context);

const api = context.window.ARMSXWebFiles;
if (!api || typeof api.openFiles !== 'function' || typeof api.openDirectory !== 'function') {
  throw new Error('web file permission API was not installed');
}

const test = api._test;
if (test.extension('GAME.CHD') !== '.chd' || test.extension('game.zip') !== '.zip') {
  throw new Error('supported extensions are not case-insensitive');
}
if (test.bestCandidate(['/x/game.bin', '/x/game.cue']) !== '/x/game.cue') {
  throw new Error('CUE must win over its companion BIN');
}
if (test.bestCandidate(['/x/game.zip', '/x/game.iso']) !== '/x/game.iso') {
  throw new Error('direct disc images must win over archive candidates');
}
let traversalRejected = false;
try {
  test.safePart('..');
} catch (_) {
  traversalRejected = true;
}
if (!traversalRejected) {
  throw new Error('parent traversal component was accepted');
}

console.log('WEB_FILE_ACCESS passed');
