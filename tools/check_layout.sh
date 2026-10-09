#!/usr/bin/env bash
set -euo pipefail

# Usage: tools/check_layout.sh [build-directory] [artifact-directory]
# QZ_LAYOUT_SKIP_BUILD=1 reuses already built objects.
# QZ_LAYOUT_PANELS="320x240 480x320" selects the panels to check.
# Requires a native simulator build made with CMake's Unix Makefiles generator.
project_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${1:-${project_dir}/build}"
artifact_dir="${2:-${build_dir}/layout-check}"
if [[ "${QZ_LAYOUT_SKIP_BUILD:-0}" != 1 ]]; then
    cmake --build "${build_dir}" --target qzdesk_screen --parallel "${QZ_LAYOUT_JOBS:-4}"
fi
python3 - "${project_dir}" "${build_dir}" "${artifact_dir}" <<'PY'
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys

root, build, artifacts = [Path(value).resolve() for value in sys.argv[1:]]
artifacts.mkdir(parents=True, exist_ok=True)
target = build / 'CMakeFiles/qzdesk_screen.dir'
if not (target / 'flags.make').is_file() or not (target / 'link.txt').is_file():
    sys.exit('Use an existing native simulator build with the Unix Makefiles generator.')
flags = {}
for line in (target / 'flags.make').read_text().splitlines():
    if ' = ' in line:
        name, value = line.split(' = ', 1)
        flags[name] = shlex.split(value)
cache = (build / 'CMakeCache.txt').read_text()
compiler = re.search(r'^CMAKE_C_COMPILER:FILEPATH=(.+)$', cache, re.M).group(1)
source = root / 'tools/check_layout.c'
obj = artifacts / 'check_layout.o'
binary = artifacts / 'check_layout'
subprocess.run([compiler, *flags.get('C_DEFINES', []), *flags.get('C_INCLUDES', []),
                *flags.get('C_FLAGS', []), '-c', str(source), '-o', str(obj)], check=True)
link = shlex.split((target / 'link.txt').read_text())
link = [str(obj) if value.endswith('/app/main.c.o') else value for value in link]
link[link.index('-o') + 1] = str(binary)
wrappers = sorted(set(re.findall(r'\b__wrap_(\w+)\s*\(', source.read_text())))
link.extend('-Wl,--wrap=' + name for name in wrappers)
subprocess.run(link, cwd=build, check=True)
failed = False
for panel in os.environ.get('QZ_LAYOUT_PANELS', '320x240 480x320').split():
    width, height = panel.split('x')
    output = artifacts / panel
    output.mkdir(exist_ok=True)
    with (output / 'report.txt').open('w') as report:
        result = subprocess.run([str(binary), width, height, str(output)], cwd=root,
                                stdout=report, stderr=subprocess.STDOUT)
    contents = (output / 'report.txt').read_text()
    print(contents, end='')
    failed |= result.returncode != 0
print('Layout reports and PPM screenshots:', artifacts)
sys.exit(1 if failed else 0)
PY
