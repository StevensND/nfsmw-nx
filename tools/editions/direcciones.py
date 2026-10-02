# -*- coding: utf-8 -*-
# Lists every guest address (0x82xxxxxx) that an application tree uses, one per line, for emparejar.py.
#
# A tree uses addresses in several places: its sources (hooks, overrides and constants), the code generator settings
# (nfsmw.toml, overrides.toml, huecos.toml, generated/rexglue.cmake), the linker order (orden_funciones.ld), the
# function partition of the code generator (generated/default/codegen.partition.json) and the weak calls of the
# generated code. The partition and the generated code only exist after running the code generator on the tree, so
# generate the reference tree first.
#
# Usage: direcciones.py [tree, default app] > addresses.txt
#        emparejar.py <PAL image> <other image> <output.tsv> < addresses.txt
import glob
import json
import os
import re
import sys

ADDRESS = re.compile(r'(?<![0-9A-Fa-f])82[0-9A-Fa-f]{6}(?![0-9A-Fa-f])')
# names split in two and joined by the preprocessor, e.g. UNIR_(__imp__sub_824F, D7C0) (see crear_arbol.py)
SPLIT = re.compile(r'(sub_)(82[0-9A-Fa-f]{0,6})(\s*,\s*)([0-9A-Fa-f]{1,6})(?![0-9A-Fa-f])')
# headers with SPIR-V or other binary data written in hexadecimal: not addresses
BINARY = ('_spirv.h', '_ps.h', '_vs.h')
WEAK_CALL = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')


def main():
    tree = sys.argv[1] if len(sys.argv) > 1 else 'app'
    found = set()

    def scan(text):
        found.update(int(x, 16) for x in ADDRESS.findall(text))
        for m in SPLIT.finditer(text):
            if len(m.group(2)) + len(m.group(4)) == 8:
                found.add(int(m.group(2) + m.group(4), 16))

    for folder, _, files in os.walk(os.path.join(tree, 'src')):
        for name in files:
            if name.endswith(('.cpp', '.h', '.hpp', '.inl', '.c')) and not name.endswith(BINARY):
                scan(open(os.path.join(folder, name), encoding='utf-8').read())
    for rel in ('CMakeLists.txt', 'nfsmw.toml', 'orden_funciones.ld', 'overrides.toml', 'huecos.toml',
                os.path.join('generated', 'rexglue.cmake')):
        path = os.path.join(tree, rel)
        if os.path.exists(path):
            scan(open(path, encoding='utf-8').read())

    partition = os.path.join(tree, 'generated', 'default', 'codegen.partition.json')
    if not os.path.exists(partition):
        sys.exit('%s not found: run the code generator on %s first' % (partition, tree))
    found.update(int(a, 16) for a in json.load(open(partition))['assignments'])
    for path in glob.glob(os.path.join(tree, 'generated', 'default', 'nfsmw_recomp.*.cpp')):
        found.update(int(x, 16) for x in WEAK_CALL.findall(open(path, encoding='utf-8').read()))

    for address in sorted(found):
        print('%08X' % address)


if __name__ == '__main__':
    main()
