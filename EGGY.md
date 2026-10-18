# Eggy host API

This fork is the engine behind Eggy's Smash Melee game. Eggy's page builds the
match in its own UI, then has the engine play it: real Melee VS, from a config
the page writes into wasm memory. The engine reports back through one callback.
The code lives in `src/pc/eggy/`. Upstream files change only where a hook is
needed, each behind `MELEE_EGGY` (see "Upstream files touched" below), so
melee-web's own page and the probe boot (`MELEE_BOOT_SCENE=vs`) behave exactly
as before.

## Turning it on

Put these in `Module.ENV` from `preRun`:

| Variable | Value |
|---|---|
| `MELEE_EGGY` | `1` |
| `MELEE_VS_ONLY` | `1` |
| `MELEE_BOOT_SCENE` | `eggy` |

`MELEE_FPS=1` still logs frame pacing. `MELEE_SEED` has no effect: each match
is seeded from its config.

The page also sets the usual fields: `Module.canvas` (16:9, so the fixed 16:9
picture fills it), `readDisc`, `discFile`, and optionally `onFrame`,
`onGraphicsPreparation` and `onAbort`. It adds `Module.eggy = { onState }`. If
it already has an AudioContext unlocked by a tap, it passes it as
`Module.meleeAudioContext`. It passes `Module.meleeAudioWorkletUrl`, or a
`Module.locateFile` that resolves `melee-audio.worklet.js`, so the audio
worklet loads from a same-origin file (see Sound). For two-phone matches it
also gives the engine its link to the other phone (see Netplay).

## Call order

1. `callMain([])`. The engine boots and parks: it renders nothing new (the
   last picture stays up), yields to the browser every frame, and calls
   `onState('parked', 0, 0)`.
2. The page writes a config (layout below) at `HEAP32[_eggy_config_buffer() >> 2]`
   and calls `_eggy_load(serial)`.
   - It returns 0 if the config is accepted. Otherwise it returns an error code
     and also calls `onState('error', code, serial)`; the engine stays parked.
   - The page may load before the first `parked`. The engine takes the config
     when it gets there.
3. The engine loads the stage and fighters (`readDisc`) and enters the VS
   scene. With the first tick of the match next, it holds: it still yields
   every frame, the simulation does not advance and nothing is drawn (the
   canvas shows black), and it calls `onState('loaded', serial, 0)`.
4. `_eggy_go()` releases it. Melee's own READY... GO! plays. `eggy_go` is
   latched: called while loading, it takes effect the moment the match is
   loaded (`loaded` is still reported first).
5. During the match the page calls `_eggy_pad(...)` for each port it drives,
   whenever its input changes or once a frame, and reads `_eggy_hud()`.
6. At GAME! (or TIME!, or after sudden death) the standings go into
   `_eggy_results()`, then `onState('ended', outcome, serial)`. If config word
   [15] > 0, Melee's results screen shows for that many seconds.
7. `onState('parked', serial, 0)`. The page goes back to step 2 with the next
   config. The same engine instance plays every match of the session.

`_eggy_abort()` ends whatever is loading or running at once and parks again.
There are no results (outcome 3, nothing else filled) and no `ended`, only the
`parked` that follows. Aborted during the results screen, it only cuts the
screen short; the match's results stand.

All exports latch requests. The game thread acts on them at its next frame
boundary or tick, so any of them may be called whenever the engine has
yielded, including from inside `onState`.

A minimal driver: Fox (human, port 1) against a level 9 Falco on Final
Destination, 4 stocks, 8 minutes, items off.

```js
let serial = 0;
Module.eggy = {
  onState(name, a, b) {
    if (name === 'parked') {
      const w = new Int32Array(48);
      w.set([1, 12345, 32, 0, 4, 8, -1, -1, -1, 0, 1, 1, 1, 100, 127, 0]);
      w.set([1, 2, 0, 0, 0, 0, 0, 0], 16);  // port 1: human Fox, costume 0
      w.set([3, 20, 0, 0, 9, 0, 0, 0], 24); // port 2: CPU Falco, level 9
      Module.HEAP32.set(w, Module._eggy_config_buffer() >> 2);
      Module._eggy_load(++serial);
    } else if (name === 'loaded') {
      Module._eggy_go();
    } else if (name === 'ended') {
      const r = new Int32Array(Module.HEAP32.buffer, Module._eggy_results(), 34);
      console.log('outcome', a, 'end frame', r[0], 'P1 placement', r[3]);
    } else if (name === 'error') {
      console.log('config refused at word', a - 1);
    }
  },
};
// Each frame: Module._eggy_pad(0, buttons, sx, sy, cx, cy, l, r);
```

`Module.HEAP32` and `Module.HEAPU8` are exported runtime views. Emscripten
replaces them when memory grows, so take them fresh at each use rather than
keeping a view across frames.

## Exports

| Function | Returns | |
|---|---|---|
| `int32_t* eggy_config_buffer(void)` | the config words | 64 words; v1 uses 48 |
| `int eggy_load(int serial)` | 0 or an error code | `serial` comes back in `onState` |
| `int eggy_go(void)` | 1 if taken | |
| `void eggy_pad(int port, int buttons, int stickX, int stickY, int substickX, int substickY, int triggerL, int triggerR)` | | port 0..3; PADStatus semantics: GameCube button bits (mask 0x1F7F), int8 sticks, uint8 triggers |
| `const int32_t* eggy_hud(void)` | the HUD words | updated every tick; also refreshed by the call |
| `const int32_t* eggy_results(void)` | the results words | |
| `int eggy_abort(void)` | 1 if there was something to abort | |
| `uint32_t eggy_checksum(void)` | the frame checksum | 0 outside a match; read it with `>>> 0` |
| `void eggy_audio(int musicVolume, int sfxVolume)` | | 0..127 each, any time |
| `int eggy_costumes(int ckind)` | how many costumes | every costume the game has |
| `int eggy_team_costume(int ckind, int team)` | the costume a team wears | what character select dresses a team in |
| `const int32_t* eggy_net_stats(void)` | the netplay stats words | see Netplay |

They are `Module._eggy_*` (EXPORTED_FUNCTIONS in `platforms/browser/CMakeLists.txt`).
The pointers index `Module.HEAP32` as `ptr >> 2`.

## onState

`Module.eggy.onState(name, a, b)`:

| name | a | b | |
|---|---|---|---|
| `parked` | serial of the last match (0 at boot) | 0 | ready for `eggy_load` |
| `loaded` | serial | 0 | held at frame 0; `eggy_go` releases |
| `ended` | outcome | serial | `eggy_results()` is filled |
| `error` | error code | serial | the config was refused; still parked |
| `net-error` | net error code | serial | netplay: no session, or it died before frame 0; `parked` follows |
| `desync` | session frame | serial | netplay: the two phones' checksums disagreed there; play goes on |
| `peer-lost` | match frame | serial | netplay: the other phone is gone mid-match; `ended` (outcome 4) follows |

An exception thrown by the callback is logged, never raised into the engine.

## Config, layout v1

All words are int32.

| Word | Field | Values |
|---|---|---|
| 0 | version | 1 |
| 1 | seed | any uint32; the game RNG is set to it as the match loads |
| 2 | stage (StKind) | 2..32 except 21 (Akaneia) and 26 (Icicle Mountain top) |
| 3 | match kind | 0 stock, 1 time |
| 4 | stocks | 1..99 for stock (time accepts 0..99 and ignores it) |
| 5 | time limit, minutes | 0..99, 0 no clock |
| 6 | item frequency | -1 off, 0..4 very low .. very high |
| 7, 8 | item mask, low and high 32 bits | bit n allows ItemKind n (table below); -1, -1 is all |
| 9 | teams | 0/1 |
| 10 | friendly fire | 0/1 |
| 11 | frozen Pokemon Stadium | 0/1 |
| 12 | music on this device | 0/1 |
| 13 | music volume | 0..127 |
| 14 | sound effects volume | 0..127 |
| 15 | results screen seconds | 0..600; 0 parks right after GAME! |
| 16 + 8p | port p (0..3): kind | 0 none, 1 human, 3 CPU |
| 17 + 8p | CharacterKind | 0..25 |
| 18 + 8p | costume | 0..`eggy_costumes(ckind)`-1 |
| 19 + 8p | team | 0 red, 1 blue, 2 green |
| 20 + 8p | CPU level | 1..9 for a CPU; 0..9, ignored, for a human |
| 21 + 8p | handicap | 0 off, 1..9 |
| 22, 23 + 8p | reserved | 0 |
| 48 | netplay | 0 this phone alone (words 49..63 are not read), 1 two phones |
| 49 | this phone's port | one of the two human ports |
| 50 | phones in the match | 2 |
| 51 | the mesh leader's port | the lowest human port; its phone hosts the session |
| 52 | session id | any uint32, distinct per match |
| 53 | input delay, frames | 0 auto (from ping), 1..4 fixed |
| 54..63 | reserved | 0 |

An empty port's other words are never read. A config needs two fighters at
least, and with teams on, two teams at least. With netplay on it needs exactly
two humans, one per phone.

The two phones of a netplay match write the same 64 words except 12..14 (this
device's sound) and 49 (this phone's port). `eggy_config_hash` is FNV-1a 32
over every other word, each word's four bytes low first; a phone whose hash
differs is refused (net error 4).

Error codes:

| Code | Meaning |
|---|---|
| 1..64 | word (code - 1) is invalid; the first one found |
| 100 | busy: not parked |
| 101 | fewer than two fighters |
| 102 | teams on, every fighter on one team |
| 103 | the engine was not started with `MELEE_EGGY` |
| 104 | netplay without exactly two humans |

Item mask bits (`ItemKind`, `src/melee/it/forward.h`):

| Bit | Item | Bit | Item | Bit | Item |
|---|---|---|---|---|---|
| 0 | Capsule | 12 | Beam Sword | 24 | Fan |
| 1 | Crate | 13 | Parasol | 25 | Fire Flower |
| 2 | Barrel | 14 | Green Shell | 26 | Super Mushroom |
| 3 | Egg | 15 | Red Shell | 27 | Poison Mushroom |
| 4 | Party Ball | 16 | Ray Gun | 28 | Hammer |
| 5 | Barrel Cannon | 17 | Freezie | 29 | Warp Star |
| 6 | Bob-omb | 18 | Food | 30 | Screw Attack |
| 7 | Mr. Saturn | 19 | Proximity Mine | 31 | Bunny Hood |
| 8 | Heart Container | 20 | Flipper | 32 | Metal Box |
| 9 | Maxim Tomato | 21 | Super Scope | 33 | Cloaking Device |
| 10 | Starman | 22 | Star Rod | 34 | Poke Ball |
| 11 | Home-Run Bat | 23 | Lip's Stick | | |

This is the mask Melee's rules carry (`StartMeleeRules.x20`). The item switch
screen's 31 switches map onto it through `lbl_803B7844` (`gm_1601.c`).

## HUD

`eggy_hud()`, 19 words, current after every tick:

| Word | |
|---|---|
| 0 | state: 0 parked, 1 loading, 2 loaded (held at frame 0), 3 fighting, 4 ended / results screen, 5 error |
| 1 | match frame: ticks since frame 0, through sudden death; frozen at the end frame once ended |
| 2 | seconds left on the clock, -1 with no clock |
| 3 + 4p | port p: present (0/1) |
| 4 + 4p | stocks |
| 5 + 4p | percent, as the game's HUD shows it |
| 6 + 4p | the fighter's CharacterKind now (18/19 follow Zelda and Sheik's changes) |

The port words are live while loaded and fighting. From GAME! they hold their
last values, and they are 0 while parked.

## Results

`eggy_results()`, 34 words, filled before `ended`:

| Word | |
|---|---|
| 0 | end frame: the match frame of GAME! / TIME! (sudden death's, if played) |
| 1 | outcome: 0 normal, 1 time out, 2 sudden death then decided, 3 aborted, 4 disconnected (netplay) |
| 2 + 8p | port p: present (0/1) |
| 3 + 8p | placement 1..4, as Melee ranks it (by score, ties shared, teammates share the team's) |
| 4 + 8p | stocks left |
| 5 + 8p | KOs |
| 6 + 8p | falls |
| 7 + 8p | self-destructs |
| 8 + 8p | damage dealt |
| 9 + 8p | damage taken |

These are read from Melee's own match end (`MatchEnd.player_standings`, filled
by `gm_80166378`). KOs are `x20`, falls `x24`, damage dealt `x40`
(`pl_80040B3C`) and damage taken `x44` (`pl_80040B64`). After a sudden death,
Melee folds its placements into the VS match's standings
(`gmVsMelee_ExitSuddenDeath`); the counts stay the VS match's, as the results
screen shows them.

## How the match is built

`GM_EGGY` is a game mode with three states (`src/pc/eggy/eggy.c`):

- **VS.** Its `on_enter` parks, then builds the `StartMeleeData` from the
  config, the way `onEnterDebugVs` (`gmvsmode.c`) and `gmVsMelee_EnterVs` do:
  - `gm_SetupRulesDefaults`, then the config's rules;
  - `is_vs`, pause off (`disable_pausing`), SD penalty -1 (Melee's default);
  - each port from `gm_SetupPlayerDefaults`, then `gm_SetupSubColors`;
  - `gm_LoadAnnouncer`;
  - the seed, last.
- **Sudden death.** After a tie, as VS mode plays it (`gm_SetupSuddenDeath`
  from the match as built).
- **Results.** Melee's results screen, ended after word [15] seconds through
  the scene's own end (`gm_801A4B60`).

The loop is Melee's scene loop, and every scene tears its heaps down the way
VS mode's CSS, VS, results cycle does. The Eggy code allocates nothing.

- **Sheik** is CKind 19 in the config. The engine passes it to the game as is,
  so she loads as Sheik, with Zelda as her transformation, as `ftMapping_list`
  pairs them. Melee's "hold A at the start to swap Zelda and Sheik" is off
  under `MELEE_EGGY` (`gmvs.c`). A button held from the last scene would
  otherwise swap her.
- **Costumes.** Every fighter can wear every costume the game has. The
  VS-only profile's cap of two (`gm_GetNumCostumesForCKind`) is lifted under
  `MELEE_EGGY`. `eggy_costumes(ckind)` gives the count.
- **Teams.** With teams on, each fighter wears its team's costume, as character
  select dresses them (`gm_80169264`, `gm_801692BC`, `gm_80169290`; Sheik
  uses Zelda's row, as she does on character select). The config's costume
  word is still validated, but not worn. `eggy_team_costume(ckind, team)`
  tells the page which costume to preload. Teammates of one character get
  Melee's shades (`gm_SetupSubColors`). Friendly fire is the config's. With
  teams off, every fighter is on team 0, as character select leaves them.
- **Items.** The frequency goes to `StartMeleeRules.item_freq`, the mask to
  `StartMeleeRules.x20`.
- **Unlocks.** No memory card is present under `MELEE_EGGY`
  (`aurora_card_set_present(false)`), so every device builds the same
  card-absent save. Every match also pins `pc_unlock_state_all()`, the state
  netplay pins: unlock predicates gate RNG draws.
- **Fixed rules.** UCF is on. Pause is off. The picture is widescreen mode 1:
  fixed 16:9, never the window's shape. The HUD is melee-pc's wide HUD.
  Nothing records or saves: the VS-mode record keeping (`gm_80162574`,
  `gm_801623A4` and the rest of `gmVsMelee_ExitVs` / `ExitResults`) is never
  called, so the save data, and with it every RNG-gating predicate, stays the
  same match after match.
- **Frozen Pokemon Stadium** follows config word [11].

## Input

Every `PADRead` in the engine goes through `__wrap_PADRead` in
`src/pc/eggy/eggy.c` (`-Wl,--wrap=PADRead`). That covers the pad alarm's
`HSD_PadRenewRawStatus`, netplay's local sample and the chat overlay. Under
`MELEE_EGGY` it returns the page's four pads in place of aurora's whole merge.
Aurora merges SDL keyboard, gamepads, touch and the GameCube adapter, so none
of those reach the game.

- All four ports read as connected.
- The page's pads count only while a match is held or running. Otherwise
  (parked, loading, results screen) every port reads neutral, so a results
  screen runs its full time.
- `eggy_load` resets the four pads to neutral.
- Netplay: only this phone's port is read, as port 0's sample, which is the one
  `net.c` takes (`capture_local_sample`). The session writes all four ports of
  every frame itself (`write_head`): this phone's and the other phone's humans
  on their ports, every other port "no controller".
- SDL's keyboard element is `#none` (`SDL_HINT_EMSCRIPTEN_KEYBOARD_ELEMENT`,
  set before the window is created), so SDL listens to no key and never
  captures or default-prevents the page's keystrokes.

During the hold, pad samples queue as usual. The scene loop's
`HSD_PadFlushQueue(LEAVE1)` runs after the hold, so the first tick reads the
newest sample and no catch-up ticks run.

A full analog press of L or R is the digital bit plus 255 on the trigger.

## Sound

- **Music on this device** (word [12] = 0): `lbAudioAx_80023F28` returns
  before any music stream starts, so no music file is ever read. No VS caller
  uses its result, so the simulation is the same either way.
- **Volumes:** words [13] and [14], and `eggy_audio()` any time (the mixer's
  music and sound-effect gains).
- **Parking** after a match stops the music and keys off every sound effect,
  as a match end does.
- **The AudioWorklet** is a static file in the release,
  `melee-audio.worklet.js`. The page names it with `Module.meleeAudioWorkletUrl`
  or resolves it with its `Module.locateFile`. With neither (melee-web's own
  page), or if the file fails to load, the inline copy loads from a `blob:` URL
  as before. Eggy's CSP (`script-src 'self'`) blocks `blob:`.
- **The AudioContext:** `Module.meleeAudioContext`, if the page set it, is used
  instead of a new one. The worklet resamples to its rate. The gesture-resume
  listeners stay.

## Checksum

`eggy_checksum()` is netplay's desync checksum, `frame_checksum` in
`src/pc/net_snapshot.c`. It folds the four pads the last tick simulated, the
RNG seed, and each fighter's position, facing, percent, action, stocks and
velocities. It is 0 outside a match, where player slots hold stale pointers.
`eggy_hud()[1]` names the frame it belongs to.

## Netplay

Two phones, one match, on melee-web's own rollback session (`src/pc/net*.c`:
the RULES/READY handshake, the ready barrier, input exchange with rollback,
time sync, auto delay, resume after an interruption, desync checks, the scene
hand-off). `src/pc/eggy/eggy_net.c` runs the steps `net_match_web.c` runs once
its link is open, minus matchmaking: the page has paired the phones already.

### What the page provides

The engine's datagrams cross two rings in wasm memory (`src/pc/net_web.c`),
exactly as for melee-web's `netplay.mjs`:

- `Module.meleeNetLinkReady`, set before the runtime starts. The engine calls it
  once with `Module.meleeNetLink` (`{buffer, tx, rx, generation, slots, bytes}`)
  in place. The page drains `tx` to the other phone and puts what arrives into
  `rx`. `datagramRing(link, link.tx)` in the release's `netplay.mjs` is the
  codec.
- After every `rx.put`, `Module._net_web_pump()`, so the engine acks on arrival.
- `Module.meleeNetplay = { status, sendNow }`:
  - `status()` returns 4 once the link to the other phone is open, -1 if it
    failed, anything else while it opens;
  - `sendNow(bytes)` (optional) sends one datagram at once, called from the
    game thread.
- The link carries datagrams of at most 512 bytes, one message each, and may
  drop or reorder them. A WebRTC data channel opened
  `{ordered: false, maxRetransmits: 0}` is the intended one. A reliable channel
  works too, at the cost of head-of-line delay.

```js
import { datagramRing } from './netplay.mjs';
let tx, rx;
Module.meleeNetplay = {
  status: () => (channel?.readyState === 'open' ? 4 : 1),
  sendNow: (bytes) => channel?.readyState === 'open' && channel.send(bytes),
};
Module.meleeNetLinkReady = () => {
  const link = Module.meleeNetLink;
  tx = datagramRing(link, link.tx);
  rx = datagramRing(link, link.rx);
  const pump = () => {
    tx.drain((bytes) => channel?.readyState === 'open' && channel.send(bytes));
    const seen = Atomics.load(tx.i32, tx.head);
    Atomics.waitAsync(tx.i32, tx.head, seen).value.then(pump);
  };
  pump();
};
channel.onmessage = ({ data }) => {
  rx?.put(new Uint8Array(data));
  Module._net_web_pump();
};
```

The link can stay open from match to match. Every match is its own session:
word [52] becomes the session id on both phones, so a late datagram of the last
match is dropped by the next.

### Call order, on both phones

1. Both pages open the link and see `parked` (or load before it).
2. Both write the match: the same words, with [48] = 1, [49] = each phone's
   own port, [50] = 2, [51] = the lowest human port, [52] = a new session id,
   [53] = the delay. Then `_eggy_load(serial)`.
3. The engine waits for `status()` 4 (20 s), connects, waits for the other
   phone's session to answer (30 s), runs the handshake (15 s) and exchanges
   the config hash. Nothing is simulated meanwhile. Any failure is
   `net-error` then `parked`.
4. The VS scene loads. Each phone holds before its first tick, tells the other
   it is loaded, and reports `loaded` once both are (the other phone gets
   120 s to load).
5. Both pages call `_eggy_go()`. The guest's go only arms the start. The
   leader's engine, holding its own go and the guest's, sends GO and releases.
   The guest releases when GO arrives. The first tick is session frame 0 on
   both: the session starts there, so no frame can be simulated before the
   release. The leader runs ahead by at most the input delay, then waits for
   the guest's first inputs.
6. The match plays under rollback. Input delay is auto from ping unless [53]
   fixes it. The host decides it in the fight's first seconds (`net_sync.c`);
   it is 2 on most links, 1 on a fast LAN.
7. At GAME! both phones have the same results, fire `ended`, show the results
   screen for [15] s in lockstep, and leave it on the same frame (the scene
   hand-off). Each then says DONE, waits up to 5 s for the other's, closes the
   session and fires `parked`.

`_eggy_pad(localPort, …)` is safe any time after the runtime is up, as solo.
Only this phone's port is read: the session sends it to the other phone and
writes both humans' ports into every frame itself.

`_eggy_abort()` leaves the session at once, even from inside a long stall
(`pc_net_leave`). The phone that aborts gets no `ended` (outcome 3). The other
one gets `peer-lost` and `ended` with outcome 4.

### Failures

| net-error | |
|---|---|
| 1 | the page's link did not open (`status()` -1, or not 4 within 20 s) |
| 2 | the session could not be opened |
| 3 | the RULES/READY handshake failed or timed out |
| 4 | the other phone's config is another match (hash or session id) |
| 5 | the other phone left, or went silent past 30 s, before frame 0 |
| 6 | the other phone never answered, or never finished loading |
| 7 | the other phone runs another wire protocol (another engine build) |

A phone that refuses a mismatched match waits 0.6 s after its own HELLO went
out before closing, so the other phone sees the mismatch as well (4, not 5).

Mid-match, a silent peer is ridden out: 15 s of stall, then 15 s of resume
(`net.c`'s reconnect phase, raised from 3 s + 3 s for phones; iOS suspends a
backgrounded tab). Past that, or on the other phone's BYE:

- `peer-lost(match frame, serial)`;
- Melee ends the match as it stands (`gm_8016B328`, `OUTCOME_TERMINATED`, so
  placements are Melee's own for the standings at that frame);
- `ended(4, serial)`, no results screen, and `parked`.

A session lost after GAME! changes nothing: the results stand, and the results
screen finishes on this phone alone.

A desync (`check_desync`, the existing checksum compare) is reported once per
session as `desync(frame, serial)`, and play goes on.

### Stats

`eggy_net_stats()`, 8 int32 words. They are refreshed every tick and every held
frame, and kept after the session ends until the next netplay `eggy_load`.

| Word | |
|---|---|
| 0 | state: 0 none, 1 waiting for the link, 2 handshaking, 3 loading and holding, 4 playing, 5 closing, 6 closed, 7 failed |
| 1 | ping, ms (smoothed round trip) |
| 2 | input delay, frames |
| 3 | rollbacks so far |
| 4 | deepest rollback, frames |
| 5 | desyncs (0/1: the first is reported, then the session stops comparing) |
| 6 | frames stalled on the other phone's input |
| 7 | link quality: 0 stable, 1 warning, 2 stalling, 3 reconnecting |

`eggy_checksum()` keeps working: it is the frame checksum of the newest
simulated frame, the same on both phones once that frame is confirmed.

### Reused and changed

Reused as is: the session, the socket over the page's link (`net_web.c`),
connect, the handshake, the reliable lanes, rollback and snapshots, time sync,
auto delay, resume, BYE, the scene hand-off, desync detection.

Changed in `src/pc/net.c` / `net_handshake.c`, as knobs that keep their
defaults unless `src/pc/eggy` sets them, so melee-web's online page is
unchanged:

- `pc_net_set_ports`: which ports the host and guest drive. `write_head` seats
  them there; every other port reads "no controller" as before.
- `pc_net_set_session_id`: both phones use the id the page gave, instead of the
  host drawing one.
- `pc_net_set_timeouts`: 15 s stall and 15 s resume.
- `pc_net_set_match_start(0)`: the handshake names frame 0 as the start, so
  checksums are compared, and the RNG reseeded per frame, from the first tick.
- `pc_net_poll_parked`: the poll the park and the hold run, which also starts
  the transmit timer (keepalives through a long load) and counts the agreed
  hold as progress.
- `pc_net_quiet_ms`, `pc_net_counters`, `pc_net_leave`: last-heard time, the
  stats above and the first desync frame, and an abort that does not wait out
  a stall.

### Determinism, Chrome against Safari

- **Floats.** The simulation is wasm scalar code, built with
  `-ffp-contract=off -fno-fast-math`, its own trig (`src/pc/libm`), no SIMD, and
  musl's libm compiled to wasm. Wasm arithmetic is IEEE-exact on every engine.
  One caveat: the bit pattern of a NaN a computation produces is left open by
  the wasm spec. A NaN in a checksummed field (position, velocity, percent)
  could read as a desync between V8 and JavaScriptCore without the simulation
  differing. None is expected.
- **Memory.** Game code is built with `-ftrivial-auto-var-init=zero`, so stack
  garbage is zero. Heap blocks are reused across scenes, and their old bytes
  depend on what each phone played before. A game read of uninitialised heap
  would differ between the phones. The desync check would show it; none is
  known.
- **Timing.** Nothing is simulated while either phone loads or holds: frame 0
  waits for both. Disc and ARAM transfers settle inside the tick that asked
  (`dvd_settle`). Audio answers the simulation asks for are journalled or
  fixed (`pc_net_audio_deaf`). Out-of-tick RNG draws are put back
  (`seed_out_of_tick_check`), and the seed is reseeded every fight frame
  (`fight_reseed`). The results screen's timer counts lockstep ticks, and its
  exit is agreed. The picture is fixed 16:9 on both phones (widescreen mode 1),
  never the canvas's shape, so anything that reads the camera's shape reads
  the same on both.
- **Per-device words.** [12] (music on this device) only decides whether a
  music stream is opened. No VS caller reads the result, and the stream's
  reads never raise the rollback barrier in the browser (`pc_audio_pumping`).
  [13] and [14] are mixer gains.
- **Not checked by the engine:** that both phones run the same engine release
  and the same disc. The wire protocol version is checked; the page should load
  one release and one disc image for both.

### Asyncify

If Asyncify is ever narrowed (`-sASYNCIFY_ONLY`), keep the paths that sleep
through `VIWaitForRetrace`: the park, including its netplay connect and close,
from `gm_801A4014` (the VS state's `on_enter`); the hold from `gm_801A4D34`;
and net.c's `wait_remote`.

## Not done

`eggy_render_size` is not built. Aurora has no public resize call, and the
render size is still the canvas's at boot.

## Upstream files touched

Each change is behind `MELEE_EGGY` (`pc_eggy_enabled()`), which is false
unless all three environment variables are set. The net.c changes are knobs
that keep their defaults unless `src/pc/eggy` sets them.

| File | Change | Why |
|---|---|---|
| `src/melee/gm/forward.h` | `GM_EGGY` before `GM_COUNT` | the mode Eggy matches run in |
| `src/melee/gm/gmscdata.c` | the `GM_EGGY` entry in the mode table | its states are `gm_Mode_Eggy_States` (`src/pc/eggy/eggy.c`) |
| `src/melee/gm/gmboot.c` | `MELEE_BOOT_SCENE=eggy` maps to `GM_EGGY` | boot straight into it |
| `src/melee/gm/gmscene.c` | `pc_eggy_scene_begin` at the start of `gm_801A4D34`, `pc_eggy_after_tick` at the end of `gm_RunSimTick` | the hold at frame 0 (before the pad-queue flush); the HUD, abort and results timer per tick |
| `src/melee/gm/gmvs.c` | Zelda/Sheik hold-A swap off | the config names her outright |
| `src/melee/gm/gm_1601.c` | `gm_GetNumCostumesForCKind` uncapped | every costume |
| `src/melee/lb/lbaudio_ax.c` | `lbAudioAx_80023F28` returns early with music off | no music file read on that device |
| `src/pc/vi.c` | the frame buffer is preserved while parked | the last picture stays up; a load or hold clears to black, so frame 0 draws into cleared buffers |
| `src/pc/audio.c` | the worklet loads from a static file first; the page's AudioContext is used | Eggy's CSP; iOS's unlocked context (any page, not only `MELEE_EGGY`) |
| `platforms/browser/main.c` | `pc_eggy_preinit()` before `aurora_initialize`; no memory card | SDL keyboard off; one card-absent save everywhere |
| `platforms/browser/pc_stubs.c` | frozen stadium from the config, UCF forced on, wide HUD | fixed rules |
| `platforms/browser/CMakeLists.txt` | `src/pc/eggy` sources, `-Wl,--wrap=PADRead`, `_eggy_*` exports | the API |
| `platforms/browser/bundle/melee-audio.worklet.js` | new | the static worklet |
| `tools/eggy/engine_release.py` | ships `melee-audio.worklet.js` | in `engine.json` with size and sha256 |
| `.github/workflows/engine.yml` | only master publishes a release; other refs upload `engine-release-<sha>` | branch builds without releases |
| `.github/workflows/ci.yml` | builds and runs `tests/eggy/eggy_config_test.c` and `tests/eggy/eggy_net_test.c` | config validation, netplay rules |
| `src/pc/net.c`, `src/pc/net.h` | the knobs under Netplay, "Reused and changed": seated ports, a shared session id, timeouts, the parked poll, counters, leave | two phones on any two ports, one session per match |
| `src/pc/net_handshake.c` | `pc_net_set_match_start` | a handshake run before any frame starts the match on frame 0 |

Tests:

- `tests/eggy/eggy_config_test.c`: config validation, every word (netplay's
  included), and the config hash, built with the host compiler in CI.
- `tests/eggy/eggy_net_test.c`: the hold's rules between two simulated phones
  (both loaded before `loaded`, the guest's go only arms, the leader releases
  on GO), roles, and the HELLO payload.
- `platforms/browser/tests/audio-worklet.test.mjs`:
  - the two worklet copies are one program;
  - the processor plays and resamples a ring;
  - `web_audio_start` picks the file, `locateFile`, or the blob, and uses the
    page's AudioContext.
