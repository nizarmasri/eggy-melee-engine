// SPDX-License-Identifier: GPL-3.0-or-later
import test from 'node:test';
import assert from 'node:assert/strict';
import { readFileSync } from 'node:fs';
import vm from 'node:vm';

// The audio output's worklet ships twice: as bundle/melee-audio.worklet.js, the
// static file a page with a 'self'-only CSP loads, and inlined in
// src/pc/audio.c for pages that name no file. These keep them one program and
// check web_audio_start picks the right one.
const root = new URL('../../../', import.meta.url);
const audioC = readFileSync(new URL('src/pc/audio.c', root), 'utf8');
const workletFile = readFileSync(new URL('platforms/browser/bundle/melee-audio.worklet.js', root), 'utf8');

const code = (text) => text.split('\n').map((l) => l.trim()).filter((l) => l && !l.startsWith('//')).join('\n');

function inlineSource() {
  const m = /const source = `([\s\S]*?)`;/.exec(audioC);
  assert.ok(m, 'audio.c inlines the worklet');
  return m[1];
}

// web_audio_start's body, as Emscripten emits the EM_JS.
function startBody() {
  const m = /EM_JS\(int, web_audio_start, \(([^)]*)\), \{\n([\s\S]*?)\n\}\);\n\/\/ clang-format on/.exec(audioC);
  assert.ok(m, 'web_audio_start is an EM_JS in audio.c');
  return m[2];
}

test('the static worklet is the inlined one', () => {
  assert.equal(code(workletFile), code(inlineSource()));
});

// The processor itself, run the way an AudioWorkletGlobalScope would.
function loadProcessor(sampleRate) {
  const registered = {};
  const scope = {
    sampleRate,
    Float32Array, Uint32Array, Atomics,
    AudioWorkletProcessor: class {},
    registerProcessor: (name, cls) => { registered[name] = cls; },
  };
  vm.runInNewContext(workletFile, scope);
  assert.ok(registered['melee-out'], 'registers melee-out');
  return registered['melee-out'];
}

function ring(frames) {
  const ringAt = 64, write = 0, read = 4;
  const buffer = new SharedArrayBuffer(ringAt + frames * 2 * 4);
  return {
    buffer, frames,
    samples: new Float32Array(buffer, ringAt, frames * 2),
    write: new Uint32Array(buffer, write, 1),
    read: new Uint32Array(buffer, read, 1),
    options: { processorOptions: { buffer, ring: ringAt, frames, write, read, rate: 32000 } },
  };
}

test('the worklet plays the ring at the game rate and stops on an empty ring', () => {
  const MeleeOut = loadProcessor(32000);
  const r = ring(256);
  for (let i = 0; i < 200; i++) {
    r.samples[2 * i] = i / 1000;
    r.samples[2 * i + 1] = -i / 1000;
  }
  Atomics.store(r.write, 0, 200);
  const node = new MeleeOut(r.options);
  const left = new Float32Array(128), right = new Float32Array(128);
  assert.equal(node.process([], [[left, right]]), true);
  for (let i = 0; i < 128; i++) {
    assert.equal(left[i], Math.fround(i / 1000));
    assert.equal(right[i], Math.fround(-i / 1000));
  }
  assert.equal(Atomics.load(r.read, 0), 128);
  node.process([], [[left, right]]);
  assert.equal(Atomics.load(r.read, 0), 199, 'stops on the last frame: the next one is not written yet');
  assert.deepEqual([...left.slice(80)], new Array(48).fill(0));
});

test('the worklet resamples to the context rate', () => {
  const MeleeOut = loadProcessor(48000);
  const r = ring(256);
  for (let i = 0; i < 256; i++) r.samples[2 * i] = r.samples[2 * i + 1] = i;
  Atomics.store(r.write, 0, 250);
  const node = new MeleeOut(r.options);
  const left = new Float32Array(120);
  node.process([], [[left]]);
  const read = Atomics.load(r.read, 0);
  assert.ok(read === 79 || read === 80, `120 output frames at 48 kHz are 80 game frames, read ${read}`);
  assert.ok(Math.abs(left[3] - 2) < 1e-5 && Math.abs(left[4] - 8 / 3) < 1e-5);
});

// web_audio_start against a fake Web Audio, returning what it asked for.
async function start({ module = {}, failFile = false } = {}) {
  const calls = { contexts: 0, modules: [], blobs: 0, nodes: [] };
  class FakeContext {
    constructor() { calls.contexts++; this.state = 'running'; this.destination = { name: 'own' }; }
    resume() {}
  }
  const worklet = (ctx) => ({
    addModule(url) {
      calls.modules.push(url);
      return failFile && !url.startsWith('blob:') ? Promise.reject(new Error('404')) : Promise.resolve();
    },
  });
  const Module = { ...module };
  if (Module.meleeAudioContext) Module.meleeAudioContext.audioWorklet = worklet(Module.meleeAudioContext);
  const scope = {
    Module,
    HEAPU8: new Uint8Array(new SharedArrayBuffer(16)),
    err: () => {},
    window: { addEventListener() {} },
    URL: { createObjectURL: () => { calls.blobs++; return 'blob:inline'; } },
    Blob: class { constructor(parts) { this.parts = parts; } },
    AudioContext: class extends FakeContext {
      constructor(...a) { super(...a); this.audioWorklet = worklet(this); }
    },
    AudioWorkletNode: class {
      constructor(ctx, name) { this.ctx = ctx; this.name = name; calls.nodes.push(this); }
      connect(dest) { this.dest = dest; }
    },
    scriptDirectory: 'https://engine.example/',
  };
  const fn = vm.runInNewContext(`(function (ring, frames, write_idx, read_idx, rate) {\n${startBody()}\n})`, scope);
  assert.equal(fn(0, 8192, 0, 0, 32000), 1);
  await new Promise((resolve) => setTimeout(resolve, 0));
  return { calls, Module };
}

test('with neither a URL nor locateFile the inline worklet loads from a blob', async () => {
  const { calls, Module } = await start();
  assert.deepEqual(calls.modules, ['blob:inline']);
  assert.equal(calls.contexts, 1);
  assert.equal(calls.nodes.length, 1);
  assert.ok(Module.meleeAudioContext && Module.meleeAudioNode);
});

test('Module.meleeAudioWorkletUrl names the static file', async () => {
  const { calls } = await start({ module: { meleeAudioWorkletUrl: '/eggy/melee-audio.worklet.js' } });
  assert.deepEqual(calls.modules, ['/eggy/melee-audio.worklet.js']);
  assert.equal(calls.blobs, 0, 'no blob: URL is made');
});

test("the page's locateFile resolves the static file", async () => {
  const asked = [];
  const locateFile = (path, prefix) => { asked.push([path, prefix]); return `/assets/${path}`; };
  const { calls } = await start({ module: { locateFile } });
  assert.deepEqual(asked, [['melee-audio.worklet.js', 'https://engine.example/']]);
  assert.deepEqual(calls.modules, ['/assets/melee-audio.worklet.js']);
  assert.equal(calls.blobs, 0);
});

test('a static file that fails to load falls back to the inline worklet', async () => {
  const { calls } = await start({ module: { meleeAudioWorkletUrl: '/missing.js' }, failFile: true });
  assert.deepEqual(calls.modules, ['/missing.js', 'blob:inline']);
  assert.equal(calls.nodes.length, 1);
});

test("the page's own AudioContext is used, not a new one", async () => {
  const shared = { state: 'running', destination: { name: 'shared' }, resume() {} };
  const { calls, Module } = await start({ module: { meleeAudioContext: shared, meleeAudioWorkletUrl: '/w.js' } });
  assert.equal(calls.contexts, 0);
  assert.equal(Module.meleeAudioContext, shared);
  assert.equal(calls.nodes[0].ctx, shared);
  assert.equal(calls.nodes[0].dest, shared.destination);
});
