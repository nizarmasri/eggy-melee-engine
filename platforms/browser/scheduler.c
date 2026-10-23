/* SPDX-License-Identifier: GPL-3.0-or-later */
#include <emscripten.h>

/* Yield to GPU promises without the nested setTimeout minimum delay.
 *
 * A page that only ever posts itself messages starves WebKit's rendering and
 * timers: a phone too slow to sleep between frames yields here and nowhere
 * else, and Safari then never put a frame of the canvas on screen (and ran a
 * two-second interval forty seconds late). So while the page is in front and no
 * animation frame has come for 50 ms, this yield resumes after the next one,
 * which lets a rendering update and the timers run first. */
// clang-format off
EM_ASYNC_JS(void, browser_yield, (void), {
  await new Promise(resolve => {
    if (!Module.browserYieldChannel) {
      const channel = new MessageChannel();
      Module.browserYieldQueue = [];
      channel.port1.onmessage = () => Module.browserYieldQueue.shift()();
      Module.browserYieldChannel = channel;
      Module.browserPaintedAt = performance.now();
      const painted = () => {
        Module.browserPaintedAt = performance.now();
        requestAnimationFrame(painted);
      };
      requestAnimationFrame(painted);
    }
    const post = () => {
      Module.browserYieldQueue.push(resolve);
      Module.browserYieldChannel.port2.postMessage(0);
    };
    if (document.visibilityState === 'visible' && performance.now() - Module.browserPaintedAt > 50) {
      requestAnimationFrame(post);
    } else {
      post();
    }
  });
});
// clang-format on
