#!/usr/bin/env python3
"""Build and run the Eggy host tests: the engine's pure parts with the host compilers.

src/pc/eggy/eggy_config.c, eggy_hold.c, eggy_report.c, src/pc/net_mesh_wire.c
and net_resync.c are pure C (no game or session state), and aurora's
lib/gfx/pipeline_rules.hpp is std-only C++, so gcc and g++ build them against
their tests in tests/eggy. tools/browser/build.py runs this before anything
else, so an engine build fails in seconds, not after the compile, when one of
these rules breaks.

  python3 tools/eggy/host_tests.py [--out DIR]

CC and CXX pick the compilers (gcc and g++ by default).
"""
import argparse
import os
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent

C_FLAGS = ['-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', f'-I{ROOT / "src"}']
CXX_FLAGS = ['-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', f'-I{ROOT / "extern/aurora/lib/gfx"}']

# (name, test, the sources it tests)
C_TESTS = [
    ('eggy_config_test', 'tests/eggy/eggy_config_test.c', ['src/pc/eggy/eggy_config.c']),
    ('eggy_net_test', 'tests/eggy/eggy_net_test.c', ['src/pc/eggy/eggy_hold.c']),
    ('eggy_mesh_test', 'tests/eggy/eggy_mesh_test.c', ['src/pc/net_mesh_wire.c']),
    ('eggy_resync_test', 'tests/eggy/eggy_resync_test.c', ['src/pc/net_resync.c']),
    ('eggy_report_test', 'tests/eggy/eggy_report_test.c', ['src/pc/eggy/eggy_report.c']),
]
CXX_TESTS = [
    ('eggy_pipeline_rules_test', 'tests/eggy/eggy_pipeline_rules_test.cpp', []),
]


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--out', type=Path, default=ROOT / 'build/eggy-host-tests')
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    cc = os.environ.get('CC', 'gcc')
    cxx = os.environ.get('CXX', 'g++')
    failed = []
    for compiler, flags, tests in ((cc, C_FLAGS, C_TESTS), (cxx, CXX_FLAGS, CXX_TESTS)):
        for name, test, sources in tests:
            exe = args.out / name
            build = [compiler, *flags, str(ROOT / test), *(str(ROOT / s) for s in sources), '-o', str(exe)]
            if subprocess.run(build, cwd=ROOT).returncode != 0:
                failed.append(f'{name} (build)')
                continue
            if subprocess.run([str(exe)], cwd=ROOT).returncode != 0:
                failed.append(name)
    if failed:
        sys.exit('eggy host tests failed: ' + ', '.join(failed))
    print(f'eggy host tests: {len(C_TESTS) + len(CXX_TESTS)} passed')


if __name__ == '__main__':
    main()
