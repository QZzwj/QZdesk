import argparse
from pathlib import Path
import re
import shlex
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('--build', default='build')
parser.add_argument('--output', default='build/verified-lcd-live')
args = parser.parse_args()
root = Path(__file__).resolve().parent.parent
build = (root / args.build).resolve()
output = (root / args.output).resolve()
output.mkdir(parents=True, exist_ok=True)
target = build / 'CMakeFiles/qzdesk_screen.dir'
flags = {}
for line in (target / 'flags.make').read_text().splitlines():
    if ' = ' in line:
        name, value = line.split(' = ', 1)
        flags[name] = shlex.split(value)
compiler = re.search(r'^CMAKE_C_COMPILER:FILEPATH=(.+)$', (build / 'CMakeCache.txt').read_text(), re.M).group(1)
obj = output / 'check_companion_live.o'
binary = output / 'check_companion_live'
subprocess.run([compiler, *flags.get('C_DEFINES', []), *flags.get('C_INCLUDES', []),
                *flags.get('C_FLAGS', []), '-c', str(root / 'tools/check_companion_live.c'), '-o', str(obj)], check=True)
link = shlex.split((target / 'link.txt').read_text())
link = [str(obj) if value.endswith('/app/main.c.o') else value for value in link]
link[link.index('-o') + 1] = str(binary)
subprocess.run(link, cwd=build, check=True)
with (output / 'report.txt').open('w') as report:
    result = subprocess.run([str(binary), str(output)], cwd=root, stdout=report, stderr=subprocess.STDOUT)
print((output / 'report.txt').read_text())
raise SystemExit(result.returncode)
