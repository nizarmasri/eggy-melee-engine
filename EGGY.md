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
worklet loads from a same-origin file (see Sound). For matches of two to four
phones it also gives the engine its links to the other phones (see Netplay).

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
| `desync` | session frame | serial | netplay: this phone's checksum and another phone's disagreed there; play goes on |
| `away` | port | F, a session frame | netplay: that port's phone was settled away; its fighter gets neutral input after frame F and the rest play on. This phone's own port: the others settled it; `ended` (outcome 4) follows |
| `peer-lost` | match frame | serial | netplay: no other phone is left mid-match; `ended` (outcome 4) follows |

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
| 48 | netplay | 0 this phone alone (words 49..63 are not read), 1 two to four phones |
| 49 | this phone's port | one of the human ports |
| 50 | phones in the match | 2..4: the number of human ports, one phone each |
| 51 | the leader's port | the lowest human port: it releases the hold, announces the auto delay and settles a silent phone away; if it goes silent, the next lowest phone still in does |
| 52 | session id | any uint32, distinct per match |
| 53 | input delay, frames | 0 auto (from ping), 1..4 fixed |
| 54..63 | reserved | 0 |

An empty port's other words are never read. A config needs two fighters at
least, and with teams on, two teams at least. With netplay on it needs two to
four humans, one per phone, and [50] says how many.

The phones of a netplay match write the same 64 words except 12..14 (this
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
| 104 | netplay humans not 2..4 (with 2..4 humans, a [50] that is not their count is word 50's error, 51) |

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
  every frame itself (`write_head`): every phone's human on its port, every
  other port "no controller".
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

Two to four phones, one match, one human port each, on melee-web's own
rollback session (`src/pc/net*.c`: input exchange with rollback, time sync,
auto delay, desync checks, the scene hand-off) over a mesh
(`src/pc/net_mesh.c`, the Slippi doubles model): every phone sends its own
port's input to every other phone, and rolls back over every remote port.
`src/pc/eggy/eggy_net.c` opens it, checks every phone's config against every
other's, and holds the match until every phone is loaded and every page said
go. The page has paired the phones and opened its links already.

### What the page provides

The engine's datagrams cross two rings in wasm memory (`src/pc/net_web.c`), as
for melee-web's `netplay.mjs`, with one difference: every datagram of a mesh
starts with one byte naming the other end.

- `Module.meleeNetLinkReady`, set before the runtime starts. The engine calls it
  once with `Module.meleeNetLink` (`{buffer, tx, rx, generation, slots, bytes,
  mesh}`) in place. `mesh` is the mesh protocol this engine speaks (10); an M2
  engine has no `mesh`, and its rings carry no port byte.
- `tx`: `[destination engine port, datagram...]`. The page sends the datagram
  (without the byte) to the phone on that port. `rx`: the page puts
  `[source engine port, datagram...]`. The 512-byte slot includes the byte, so
  a datagram is at most 511 bytes. The page adds whatever per-link header it
  needs (epoch, sequence) and strips it again; the engine never sees it.
- After every `rx.put`, `Module._net_web_pump()`. It stamps and queues what
  arrived (the round trip is timed from there) and, if the game thread has not
  sent for 50 ms (a load), answers with a keepalive. It does not touch the
  simulation.
- `Module.meleeNetplay = { status, sendNow }`:
  - `status(port)` returns 4 once the link to the phone on that engine port is
    open, -1 if it failed, anything else while it opens;
  - `sendNow(bytes)` sends one datagram at once, from the game thread, `bytes`
    starting with the destination port byte as in `tx`. A mesh sends every
    datagram this way (the game thread is the page's), so the `tx` ring only
    carries what a missing `sendNow` leaves there.
- The links carry datagrams of at most 511 bytes, one message each, and may
  drop or reorder them. A WebRTC data channel per pair of phones, opened
  `{ordered: false, maxRetransmits: 0}`, is the intended link.

```js
import { datagramRing } from './netplay.mjs';
// channels[port]: the data channel to the phone on that engine port.
let tx, rx;
const sendTo = (bytes) => {
  const ch = channels[bytes[0]];
  if (ch?.readyState === 'open') ch.send(bytes.subarray(1)); // plus the page's own header
};
Module.meleeNetplay = {
  status: (port) => (channels[port]?.readyState === 'open' ? 4 : 1),
  sendNow: sendTo,
};
Module.meleeNetLinkReady = () => {
  const link = Module.meleeNetLink;
  tx = datagramRing(link, link.tx);
  rx = datagramRing(link, link.rx);
  const pump = () => {
    tx.drain(sendTo);
    const seen = Atomics.load(tx.i32, tx.head);
    if (Atomics.waitAsync) {
      const r = Atomics.waitAsync(tx.i32, tx.head, seen);
      (r.async ? r.value : Promise.resolve()).then(pump);
    } else {
      setTimeout(pump, 2);
    }
  };
  pump();
};
for (const [port, ch] of Object.entries(channels)) {
  ch.onmessage = ({ data }) => {
    const bytes = new Uint8Array(data); // minus the page's own header
    const slot = new Uint8Array(bytes.length + 1);
    slot[0] = Number(port);
    slot.set(bytes, 1);
    rx?.put(slot);
    Module._net_web_pump();
  };
}
```

The links can stay open from match to match. Every match is its own session:
word [52] becomes the session id on every phone, so a late datagram of the
last match is dropped by the next.

### Call order, on every phone

1. Every page opens its links to the others and sees `parked` (or loads before
   it).
2. Every page writes the match: the same words, with [48] = 1, [49] = its own
   port, [50] = the number of humans, [51] = the lowest human port, [52] = a new
   session id, [53] = the delay. Then `_eggy_load(serial)`.
3. The engine waits for `status(port)` 4 for every other human port (20 s),
   opens the mesh, waits for every other phone to answer (30 s) and for every
   other phone's HELLO, the config hash (30 s). Nothing is simulated meanwhile.
   Any failure is `net-error` then `parked`.
4. The VS scene loads. Each phone holds before its first tick, tells the others
   it is loaded, and reports `loaded` once every phone still in is (the others
   get 120 s to load).
5. Every page calls `_eggy_go()`; each phone tells the others (ARMED). The
   leader, holding its own go and every other phone's, sends GO and releases;
   the rest release when GO arrives. The first tick is session frame 0 on every
   phone. A phone that has run ahead waits for the others' first inputs.
6. The match plays under rollback over every remote port. Input delay is auto
   from the worst phone's ping unless [53] fixes it; the leader decides it in
   the fight's first seconds (`net_sync.c`). Time sync slows each phone for the
   one it runs furthest ahead of.
7. At GAME! every phone has the same results, fires `ended`, shows the results
   screen for [15] s in lockstep, and leaves it on the same frame (the scene
   hand-off: the latest frame any phone asked for, plus 20). Each then says
   DONE, waits up to 5 s for the others', closes the session and fires
   `parked`.

`_eggy_pad(localPort, …)` is safe any time after the runtime is up, as solo.
Only this phone's port is read: the session sends it to the others and writes
every human port into every frame itself.

`_eggy_abort()` leaves the session at once, even from inside a stall
(`pc_net_leave`). The phone that aborts gets no `ended` (outcome 3). The others
settle it away (below) and play on; with no other phone left, a phone gets
`peer-lost` and `ended` with outcome 4.

### Away

From the hold on, a phone that goes silent is settled away. Silent means: its
BYE arrived, or nothing has been heard from it and nothing new of its input has
reached anyone (relayed, or in another phone's report) for 1.5 s; while it is
still loading a scene (before that scene's first frames of its input), 120 s.

- The acting leader settles it: the lowest human port not settled and not
  silent to that phone. If the leader itself goes silent, the next lowest phone
  still in takes over by the same rule.
- F, the last frame of its input the match uses, is the newest frame of it any
  live phone holds. Phones share what they hold of a quiet port (relays, and
  every packet's report of what it holds), and the leader waits until it holds
  all of it and the number has not moved for 250 ms.
- The leader sends AWAY(port, F) to every phone, the settled one included.
  Every phone feeds that port neutral input from F + 1 for the rest of the
  match (a frame already simulated past F on anything else is rolled back) and
  fires `away(port, F)`. The match never stalls on that phone again.
- A phone told it was settled (it was frozen, or its link was) fires
  `away(its own port, F)`, leaves the session, and ends the match where it
  stands: `ended` with outcome 4. A phone that comes back is not resumed.
- A phone left with no other phone ends the match the same way, after
  `peer-lost(match frame, serial)`. Two phones therefore behave as in M2: the
  one left gets `away` for the other, `peer-lost`, then `ended` (outcome 4).
- The match ends where it stands through Melee's own early end
  (`gm_8016B328`, `OUTCOME_TERMINATED`: placements are Melee's for the
  standings at that frame), with no results screen.
- In the hold, a phone settled away is not waited for. One with no other phone
  left fails with net-error 5.

A desync (each phone's checksums against every other's) is reported once per
session as `desync(frame, serial)`, and play goes on.

### Failures

| net-error | |
|---|---|
| 1 | a link did not open (`status(port)` -1, or not 4 within 20 s) |
| 2 | the session could not be opened |
| 3 | not every phone's HELLO arrived within 30 s |
| 4 | another phone's config is another match (hash or session id) |
| 5 | another phone left before the match was agreed, every other phone went before frame 0, or the others settled this one away in the hold |
| 6 | a phone never answered, or never finished loading |
| 7 | a phone runs another wire protocol (another engine build) |

A phone that refuses a mismatched match waits 0.6 s after its own HELLO went
out before closing, so the others see the mismatch as well (4, not 5).

Every mesh datagram carries protocol 10 in its second byte, where an M2 engine
reads its own 9: a mesh phone that hears an M2 one fails with 7. Until a phone
answers in the mesh's protocol, the mesh also sends it a 16-byte BYE in the M2
wire's own format, carrying protocol 10, which an M2 engine refuses as another
version: it fails with 7 too (its page delivering it without the port byte).

### Stats

`eggy_net_stats()`, 16 int32 words. They are refreshed every tick and every held
frame, and kept after the session ends until the next netplay `eggy_load`.

| Word | |
|---|---|
| 0 | state: 0 none, 1 waiting for the links, 2 handshaking, 3 loading and holding, 4 playing, 5 closing, 6 closed, 7 failed |
| 1 | ping, ms: the worst phone's smoothed round trip |
| 2 | input delay, frames |
| 3 | rollbacks so far |
| 4 | deepest rollback, frames |
| 5 | desyncs (0/1: the first is reported, then the session stops comparing) |
| 6 | frames stalled on the other phones' input |
| 7 | link quality: 0 stable, 1 warning, 2 stalling |
| 8 + port | that port's phone: 0 not a phone of the match, 1 this phone, 2 not heard from yet, 3 playing, 4 silent (about to be settled), 5 settled away, 6 another engine build |
| 12 + port | ping to that port's phone, ms, -1 none |

`eggy_checksum()` keeps working: it is the frame checksum of the newest
simulated frame, the same on every phone once that frame is confirmed.

### Desync resync: not done (M3b)

A desync is reported, not repaired. Moving the leader's state to the others
(`eggy_snapshot_size/write/read`, 60 KB pieces over a reliable bulk lane) is
the next step.

### Reused and changed

Reused as is: rollback and snapshots, prediction, the rollback barrier, time
sync and auto delay (`net_sync.c`), the scene hand-off, desync detection, the
page's link (`net_web.c`), the stats.

Changed in `src/pc/net.c`, behind `net.mesh`, which only `pc_net_mesh_connect`
sets, so melee-web's two-peer sessions run exactly as before:

- The remote player is a slot (`net.c` "remote slots"): an input ring, a
  contiguous mark and, once settled, the last real frame. A plain session has
  one slot and behaves as before; a mesh has one per other phone. Prediction,
  the snapshot decision, the confirmed frame and `write_head` run over every
  slot.
- `net_mesh.c` replaces the two-peer transport (send, receive, the receive
  thread, the transmit timer, acks, the reliable lanes, RULES/READY, resume)
  with the mesh's, on the game thread only.
- The scene hand-off waits for every phone still in and leaves on the latest
  frame any of them asked for, plus the lead.
- The delay announcer is the acting leader; the time-sync sample is the pair
  of the phone this one runs furthest ahead of.
- `pc_net_quiet_ms`, `pc_net_counters`, `pc_net_leave`, `pc_net_poll_parked`
  serve a mesh too. M2's knobs (`pc_net_set_ports`, `pc_net_set_session_id`,
  `pc_net_set_timeouts`, `pc_net_set_match_start`) stay for the two-peer
  session; a mesh takes its ports, session, seed and first frame at connect.

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
  would differ between phones. The desync check would show it; none is
  known.
- **Timing.** Nothing is simulated while any phone loads or holds: frame 0
  waits for all of them. Disc and ARAM transfers settle inside the tick that asked
  (`dvd_settle`). Audio answers the simulation asks for are journalled or
  fixed (`pc_net_audio_deaf`). Out-of-tick RNG draws are put back
  (`seed_out_of_tick_check`), and the seed is reseeded every fight frame
  (`fight_reseed`). The results screen's timer counts lockstep ticks, and its
  exit is agreed. The picture is fixed 16:9 on every phone (widescreen mode 1),
  never the canvas's shape, so anything that reads the camera's shape reads
  the same on both.
- **Per-device words.** [12] (music on this device) only decides whether a
  music stream is opened. No VS caller reads the result, and the stream's
  reads never raise the rollback barrier in the browser (`pc_audio_pumping`).
  [13] and [14] are mixer gains.
- **Not checked by the engine:** that every phone runs the same engine release
  and the same disc. The wire protocol version is checked; the page should load
  one release and one disc image for all of them.

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
| `.github/workflows/ci.yml` | builds and runs `tests/eggy/eggy_config_test.c`, `tests/eggy/eggy_net_test.c` and `tests/eggy/eggy_mesh_test.c` | config validation, netplay rules |
| `src/pc/net.c`, `src/pc/net.h` | the knobs under Netplay, "Reused and changed": remote slots, the mesh session (`pc_net_mesh_*`), the scene hand-off over every phone, the parked poll, counters, leave | two to four phones on any ports, one session per match |
| `src/pc/net_mesh.c`, `src/pc/net_mesh_wire.c` | new | the mesh transport, its wire format and its away rules |
| `src/pc/net_reliable.c`, `src/pc/net_sync.c` | a mesh's reliable messages go to every phone; its delay announcer is the acting leader | the same callers on a mesh |
| `src/pc/net_web.c` | `Module.meleeNetLink.mesh` | the page tells an M3 engine from an M2 one |
| `src/pc/net_handshake.c` | `pc_net_set_match_start` | a handshake run before any frame starts the match on frame 0 |

Tests:

- `tests/eggy/eggy_config_test.c`: config validation, every word (netplay's
  included), and the config hash, built with the host compiler in CI.
- `tests/eggy/eggy_net_test.c`: the hold's rules between two, three and four
  simulated phones (all loaded before `loaded`, a go only arms, the leader
  releases on GO, a phone settled away is not waited for, a leader settled
  away before its GO is replaced), roles, and the HELLO payload.
- `tests/eggy/eggy_mesh_test.c`: the mesh's wire codec and the ring's port
  byte (the 512 bytes include it), the M2 probe, the leader and settlement
  rules, and a simulated mesh of three and four phones: one going silent, the
  leader going (the next one settles it), two at once, a port only reachable
  through relays (no false settle; F is the frame the others hold), a frozen
  phone coming back (it is evicted, and what it decided is ignored), a BYE,
  and a phone loading its scene (load_ms, not away_ms).
- `platforms/browser/tests/audio-worklet.test.mjs`:
  - the two worklet copies are one program;
  - the processor plays and resamples a ring;
  - `web_audio_start` picks the file, `locateFile`, or the blob, and uses the
    page's AudioContext.
