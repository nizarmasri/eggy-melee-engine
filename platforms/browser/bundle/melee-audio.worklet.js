// SPDX-License-Identifier: GPL-3.0-or-later
// The engine's audio output (src/pc/audio.c web_audio_start): reads the mix
// from the ring in the engine's shared wasm memory on the browser's audio
// thread, resampling from the game's 32 kHz. A static file so a page whose CSP
// allows only its own origin's scripts can load it; audio.c inlines the same
// code (blob: URL) for pages that name no file. Keep the two identical
// (platforms/browser/tests/audio-worklet.test.mjs).
class MeleeOut extends AudioWorkletProcessor {
  constructor(options) {
    super();
    const o = options.processorOptions;
    this.ring = new Float32Array(o.buffer, o.ring, o.frames * 2);
    this.w = new Uint32Array(o.buffer, o.write, 1);
    this.r = new Uint32Array(o.buffer, o.read, 1);
    this.mask = o.frames - 1;
    this.step = o.rate / sampleRate; /* game frames per output frame */
    this.pos = 0;
  }
  process(inputs, outputs) {
    const [left, right] = outputs[0];
    const ring = this.ring, mask = this.mask;
    let read = Atomics.load(this.r, 0);
    const write = Atomics.load(this.w, 0);
    for (let i = 0; i < left.length; i++) {
      if (((write - read) >>> 0) < 2) { left[i] = 0; if (right) right[i] = 0; continue; }
      const a = (read & mask) * 2, b = ((read + 1) & mask) * 2, t = this.pos;
      left[i] = ring[a] + (ring[b] - ring[a]) * t;
      if (right) right[i] = ring[a + 1] + (ring[b + 1] - ring[a + 1]) * t;
      this.pos += this.step;
      while (this.pos >= 1) { this.pos -= 1; read = (read + 1) >>> 0; }
    }
    Atomics.store(this.r, 0, read);
    return true;
  }
}
registerProcessor('melee-out', MeleeOut);
