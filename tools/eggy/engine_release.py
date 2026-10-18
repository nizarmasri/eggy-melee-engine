#!/usr/bin/env python3
"""Collect the disc-free engine release for Eggy, with its engine.json manifest.

Runs after tools/browser/build.py (.github/workflows/engine.yml). It takes
what tools/browser/bundle.py would put in the web app's folder, minus
melee.pak, so the release is code only:

- melee_browser.js and melee_browser.wasm (DWARF and producers stripped the
  way bundle.py strips them, function names kept), each with a brotli .br;
- any other script the link emitted beside them (pthread or worklet loaders);
- initial_pipeline_cache.db.gz, the shader seed the page installs as
  /initial_pipeline_cache.db (tools/browser/vs_pipeline_cache.db.gz);
- melee-audio.worklet.js, the audio output's AudioWorklet as a static file,
  for a page whose CSP allows no blob: script (src/pc/audio.c);
- the host page's modules from platforms/browser/bundle (and coi-sw.js,
  gpu-preflight.mjs), with index.html as the reference page. vendor/sentry.mjs
  is left out: it is the third-party Sentry SDK, imported only when a build
  has a DSN, and a release is one flat folder of assets.

engine.json lists every file with its size and sha256. "threads" is read from
the wasm itself: a shared memory means the page must be cross-origin isolated.

  python3 tools/eggy/engine_release.py --commit SHA --out DIR [--logs DIR]

--logs also gets what the page has to provide: the wasm's imports and exports
and a scan of the JS glue (Module fields, workers, blob: URLs, eval).
"""
import argparse
import datetime
import hashlib
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/browser'))
from common import BUILD, SDK  # noqa: E402

RUNTIME = BUILD / 'runtime/platforms/browser'
BROWSER = ROOT / 'platforms/browser'
ENTRY, WASM = 'melee_browser.js', 'melee_browser.wasm'

# bundle.py's wasm-opt features: the ones melee_browser links with. Not
# --all-features, which lets wasm-opt re-encode imports in forms browsers
# reject ("Invalid import kind 127").
FEATURES = ['--mvp-features', '--enable-threads', '--enable-bulk-memory', '--enable-bulk-memory-opt',
            '--enable-call-indirect-overlong', '--enable-multivalue', '--enable-mutable-globals',
            '--enable-nontrapping-float-to-int', '--enable-reference-types', '--enable-sign-ext']

# The host page: no game data in any of these.
HOST_MODULES = ['app.mjs', 'pak-reader.mjs', 'netplay.mjs', 'controller-view.mjs', 'gc-adapter.mjs',
                'lzma.mjs', 'lzma.worker.mjs', 'pak-prefetch.worker.mjs', 'telemetry.mjs', 'index.html',
                'melee-audio.worklet.js']
PLATFORM_FILES = ['coi-sw.js', 'gpu-preflight.mjs']

# What the glue scan looks for, with a short reason each.
GLUE_PATTERNS = {
    'new Worker': r'new Worker\(',
    'createObjectURL (blob: URLs)': r'createObjectURL',
    'blob: literal': r'["\']blob:',
    'eval(': r'(?<![\w.])eval\(',
    'new Function': r'new Function\b',
    'SharedArrayBuffer': r'SharedArrayBuffer',
    'crossOriginIsolated': r'crossOriginIsolated',
    'audioWorklet / addModule': r'audioWorklet|addModule\(',
    'AudioContext': r'AudioContext',
    'resume() on audio': r'\.resume\(\)',
    'importScripts': r'importScripts\(',
    'locateFile': r'locateFile',
    'instantiateWasm': r'instantiateWasm',
    'mainScriptUrlOrBlob': r'mainScriptUrlOrBlob',
}


def leb(data, pos):
    result = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        result |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return result, pos


def wasm_name(data, pos):
    length, pos = leb(data, pos)
    return data[pos:pos + length].decode('utf-8', 'replace'), pos + length


def wasm_limits(data, pos):
    flags = data[pos]
    pos += 1
    _, pos = leb(data, pos)  # minimum
    if flags & 0x01:
        _, pos = leb(data, pos)  # maximum
    if flags & 0x08:
        _, pos = leb(data, pos)  # custom page size
    return flags, pos


def wasm_interface(data):
    """(imports, exports, shared_memory) from a wasm binary's sections."""
    assert data[:4] == b'\0asm', 'not a wasm module'
    kinds = {0: 'func', 1: 'table', 2: 'memory', 3: 'global', 4: 'tag'}
    imports, exports, shared = [], [], False
    pos = 8
    while pos < len(data):
        section = data[pos]
        size, pos = leb(data, pos + 1)
        end = pos + size
        if section == 2:  # imports
            count, at = leb(data, pos)
            for _ in range(count):
                module, at = wasm_name(data, at)
                field, at = wasm_name(data, at)
                kind = data[at]
                at += 1
                if kind == 0:
                    _, at = leb(data, at)
                elif kind == 1:
                    _, at = wasm_limits(data, at + 1)
                elif kind == 2:
                    flags, at = wasm_limits(data, at)
                    shared |= bool(flags & 0x02)
                elif kind == 3:
                    at += 2
                elif kind == 4:
                    _, at = leb(data, at + 1)
                imports.append(f'{module}.{field} {kinds.get(kind, kind)}')
        elif section == 5:  # memories defined in the module
            count, at = leb(data, pos)
            for _ in range(count):
                flags, at = wasm_limits(data, at)
                shared |= bool(flags & 0x02)
        elif section == 7:  # exports
            count, at = leb(data, pos)
            for _ in range(count):
                field, at = wasm_name(data, at)
                kind = data[at]
                _, at = leb(data, at + 1)
                exports.append(f'{field} {kinds.get(kind, kind)}')
        pos = end
    return imports, exports, shared


def scan_glue(text):
    lines = []
    reads = sorted(set(re.findall(r'Module\[["\']([A-Za-z_$][\w$]*)["\']\]', text)) |
                   set(re.findall(r'\bModule\.([A-Za-z_$][\w$]*)', text)))
    lines.append(f'Module fields the glue reads or writes ({len(reads)}):')
    lines.append('  ' + ' '.join(reads))
    for label, pattern in GLUE_PATTERNS.items():
        hits = [m.start() for m in re.finditer(pattern, text)]
        lines.append(f'\n{label}: {len(hits)}')
        for at in hits[:12]:
            line_no = text.count('\n', 0, at) + 1
            excerpt = text[max(0, at - 110):at + 140].replace('\n', ' ')
            lines.append(f'  L{line_no}: …{excerpt}…')
    return '\n'.join(lines) + '\n'


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for chunk in iter(lambda: f.read(1 << 20), b''):
            digest.update(chunk)
    return digest.hexdigest()


def brotli(path):
    subprocess.run(['brotli', '-q', '11', '-k', '-f', str(path)], check=True)
    return path.with_name(path.name + '.br')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--commit', required=True, help='the commit this engine was built from')
    parser.add_argument('--out', required=True, help='the release folder (emptied first)')
    parser.add_argument('--logs', help='also write the wasm interface and glue scan here')
    args = parser.parse_args()
    out = Path(args.out).resolve()
    if out.exists():
        shutil.rmtree(out)
    out.mkdir(parents=True)

    print('link output:', *sorted(p.name for p in RUNTIME.iterdir() if p.is_file()))
    subprocess.run([SDK / 'upstream/bin/wasm-opt', '--strip-dwarf', '--strip-producers', *FEATURES,
                    RUNTIME / WASM, '-o', out / WASM], check=True)
    # The glue, plus any worker or worklet script the link wrote beside it.
    scripts = sorted(p for p in RUNTIME.iterdir() if p.is_file() and p.suffix in ('.js', '.mjs'))
    if RUNTIME / ENTRY not in scripts:
        raise SystemExit(f'{RUNTIME / ENTRY} is missing: did build.py link melee_browser?')
    for script in scripts:
        shutil.copy2(script, out / script.name)
    brotli(out / ENTRY)
    brotli(out / WASM)
    shutil.copy2(ROOT / 'tools/browser/vs_pipeline_cache.db.gz', out / 'initial_pipeline_cache.db.gz')
    for name in HOST_MODULES:
        shutil.copy2(BROWSER / 'bundle' / name, out / name)
    for name in PLATFORM_FILES:
        shutil.copy2(BROWSER / name, out / name)

    imports, exports, threads = wasm_interface((out / WASM).read_bytes())
    files = {p.name: {'size': p.stat().st_size, 'sha256': sha256(p)}
             for p in sorted(out.iterdir()) if p.is_file()}
    manifest = {
        'id': f'engine-{args.commit[:7]}',
        'commit': args.commit,
        'built': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
        'threads': threads,
        'entry': ENTRY,
        'wasm': WASM,
        'files': files,
    }
    (out / 'engine.json').write_text(json.dumps(manifest, indent=2) + '\n')

    rows = [f'| `{name}` | {info["size"]:,} | `{info["sha256"][:16]}…` |' for name, info in files.items()]
    notes = '\n'.join([
        f'Engine `{manifest["id"]}`, built from {args.commit} with no disc: code only.',
        '',
        f'Threads (shared memory, needs a cross-origin isolated page): **{"yes" if threads else "no"}**.',
        '',
        '| File | Bytes | sha256 |', '|---|---:|---|', *rows, '',
    ])
    out.with_name(out.name + '-notes.md').write_text(notes)
    print(notes)

    if args.logs:
        logs = Path(args.logs)
        logs.mkdir(parents=True, exist_ok=True)
        (logs / 'wasm-imports.txt').write_text('\n'.join(imports) + '\n')
        (logs / 'wasm-exports.txt').write_text('\n'.join(exports) + '\n')
        report = scan_glue((out / ENTRY).read_text(errors='replace'))
        (logs / 'glue-report.txt').write_text(report)
        print(f'{len(imports)} wasm imports, {len(exports)} exports; glue scan:\n{report}')


if __name__ == '__main__':
    main()
