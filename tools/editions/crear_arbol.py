# -*- coding: utf-8 -*-
# Creates the build tree for another edition of the game (app_<edition>) from app/.
#
# Each different default.xex is a different program and needs its own NRO. Our code (app/src, the codegen
# toml files and the linker function order) is written against the Spanish PAL executable. This step
# copies what the build needs and translates every 82xxxxxx address with the table from
# tools/editions/emparejar.py. app/ is not touched.
#
# app_<edition> sits next to app/: the relative paths (../assets, ../tools, the SDK) resolve the same and
# the build objects keep the same names. That is why PGO finds its .gcda files under the same name.
#
# The partition of functions across files (generated/default/codegen.partition.json) is seeded with the
# translated Spanish one: the codegen honors it, and every file comes out with the same functions in the
# same order. Without a codegen of app/ (only another edition's executable at hand), the Spanish partition
# and its weak calls come from tools/editions/referencia; --guardar-referencia refreshes that copy.
#
# Usage: py tools/editions/crear_arbol.py <edition> <table.tsv> <xex relative to app/> [OLD=NEW ...]
#          [--parejas <json>] [--guardar-referencia]
#   OLD=NEW: XXH3 fingerprints of shaders that change in that edition (kHuellaBrightPass, kHuellaCielo...).
#            Without them, tools/editions/<edition>/huellas.txt is read if it exists.
import glob
import json
import os
import re
import shutil
import sys

RAIZ = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
APP = os.path.join(RAIZ, 'app')
EDICIONES = os.path.join(RAIZ, 'tools', 'editions')
REFERENCIA = os.path.join(EDICIONES, 'referencia')
PAT = re.compile(r'(?<![0-9A-Fa-f])82[0-9A-Fa-f]{6}(?![0-9A-Fa-f])')
# headers with SPIR-V or other binary data written in hexadecimal: not addresses
BINARIOS = ('_spirv.h', '_ps.h', '_vs.h')
SEGURAS = ('exacta', 'misma direccion', 'constante', 'referencias')  # 'referencias': from datos_por_referencias.py


def cargar_tabla(ruta):
    seguras, todas = {}, {}
    for linea in open(ruta, encoding='utf-8').read().splitlines()[1:]:
        pal, seccion, otra, estado = linea.split('\t')
        if otra == '-':
            continue
        todas[int(pal, 16)] = int(otra, 16)
        if estado.startswith(SEGURAS):
            seguras[int(pal, 16)] = int(otra, 16)
    return seguras, todas


# Names split in two to be pasted with ## (NFSMW_ESCENARIO_UNIR_(__imp__sub_824F, D7C0)): that way
# tools/llamadas_directas.py does not count them as hooked. PAT does not see them; without translating them,
# the NRO of another edition does not link or, worse, calls another function that in that edition sits
# exactly at the Spanish address.
PARTIDA = re.compile(r'(sub_)(82[0-9A-Fa-f]{0,6})(\s*,\s*)([0-9A-Fa-f]{1,6})(?![0-9A-Fa-f])')


def traducir_texto(texto, mapa, faltan):
    def cambiar(m):
        viejo = m.group()
        d = int(viejo, 16)
        if d not in mapa:
            faltan.add(viejo.upper())
            return viejo
        nuevo = '%08X' % mapa[d]
        return nuevo.lower() if any(c in 'abcdef' for c in viejo) else nuevo

    def cambiar_partida(m):
        alta, baja = m.group(2), m.group(4)
        if len(alta) + len(baja) != 8:
            return m.group()
        d = int(alta + baja, 16)
        if d not in mapa:
            faltan.add((alta + baja).upper())
            return m.group()
        nuevo = '%08X' % mapa[d]
        return m.group(1) + nuevo[:len(alta)] + m.group(3) + nuevo[len(alta):]
    return PARTIDA.sub(cambiar_partida, PAT.sub(cambiar, texto))


def cargar_referencia(guardar):
    """The function partition of the Spanish build and the functions it calls through their weak alias.

    Both come from app/generated/default after a codegen of app/. Without it (only another edition's
    executable at hand) they come from tools/editions/referencia, a copy made with --guardar-referencia:
    addresses and file numbers only, nothing of the game's code.
    """
    generado = os.path.join(APP, 'generated', 'default')
    if os.path.exists(os.path.join(generado, 'codegen.partition.json')):
        reparto = json.load(open(os.path.join(generado, 'codegen.partition.json')))
        debil = re.compile(r'(?<![\w])sub_([0-9A-F]{8})\(ctx, base\);')
        debiles = set()
        for ruta in glob.glob(os.path.join(generado, 'nfsmw_recomp.*.cpp')):
            debiles.update(debil.findall(open(ruta, encoding='utf-8').read()))
        debiles = sorted(debiles)
        if guardar:
            os.makedirs(REFERENCIA, exist_ok=True)
            with open(os.path.join(REFERENCIA, 'codegen.partition.json'), 'w', encoding='utf-8', newline='\n') as f:
                f.write(json.dumps(reparto, indent=2) + '\n')
            with open(os.path.join(REFERENCIA, 'debiles.txt'), 'w', encoding='utf-8', newline='\n') as f:
                f.write('\n'.join(debiles) + '\n')
        return reparto, debiles
    if guardar:
        raise SystemExit('--guardar-referencia needs app/generated/default (run tools/codegen.sh app first)')
    if not os.path.exists(os.path.join(REFERENCIA, 'codegen.partition.json')):
        raise SystemExit('no reference: run tools/codegen.sh app, or restore tools/editions/referencia')
    print('referencia: tools/editions/referencia (no app/generated/default)')
    reparto = json.load(open(os.path.join(REFERENCIA, 'codegen.partition.json')))
    debiles = open(os.path.join(REFERENCIA, 'debiles.txt'), encoding='utf-8').read().split()
    return reparto, debiles


def main():
    edicion, tabla, xex = sys.argv[1:4]
    resto = sys.argv[4:]
    # --parejas <json>: {"parejas": {PAL: other}} fixes functions that emparejar.py could not place (code
    # changed around them), and {"nuevas": {other: file}} places unpaired functions where they do not
    # displace the others.
    correcciones = {}
    if '--parejas' in resto:
        i = resto.index('--parejas')
        correcciones = json.load(open(resto[i + 1], encoding='utf-8'))
        del resto[i:i + 2]
    # --guardar-referencia: also write the reference data below to tools/editions/referencia, to commit it
    guardar_referencia = '--guardar-referencia' in resto
    if guardar_referencia:
        resto.remove('--guardar-referencia')
    huellas = [a.split('=') for a in resto]
    if not huellas and os.path.exists(os.path.join(EDICIONES, edicion, 'huellas.txt')):
        # tools/editions/<edition>/huellas.txt: the same OLD=NEW pairs, one per line, # for comments
        for linea in open(os.path.join(EDICIONES, edicion, 'huellas.txt'), encoding='utf-8'):
            linea = linea.split('#')[0].strip()
            if linea:
                huellas.append(linea.split('='))
    reparto, debiles = cargar_referencia(guardar_referencia)
    seguras, todas = cargar_tabla(tabla)
    todas.update({int(p, 16): int(o, 16) for p, o in correcciones.get('parejas', {}).items()})
    destino = os.path.join(RAIZ, 'app_' + edicion)
    os.makedirs(os.path.join(destino, 'generated', 'default'), exist_ok=True)

    faltan = set()
    cambios = 0
    escritos = []

    # only what changes is written: that way an existing build recompiles only those files
    def escribir(rel, texto):
        ruta = os.path.join(destino, rel)
        os.makedirs(os.path.dirname(ruta), exist_ok=True)
        if os.path.exists(ruta) and open(ruta, encoding='utf-8', newline='').read() == texto:
            return
        open(ruta, 'w', encoding='utf-8', newline='').write(texto)
        escritos.append(rel)

    # sources: every address must have a safe translation. Files no longer in app/src are deleted.
    for d, _, fs in os.walk(os.path.join(destino, 'src')):
        for f in fs:
            rel = os.path.relpath(os.path.join(d, f), destino)
            if not os.path.exists(os.path.join(APP, rel)):
                os.remove(os.path.join(d, f))
    for d, _, fs in os.walk(os.path.join(APP, 'src')):
        for f in fs:
            origen = os.path.join(d, f)
            rel = os.path.relpath(origen, APP)
            if f.endswith(BINARIOS) or not f.endswith(('.cpp', '.h', '.hpp', '.inl', '.c')):
                copia = os.path.join(destino, rel)
                os.makedirs(os.path.dirname(copia), exist_ok=True)
                if not os.path.exists(copia) or open(copia, 'rb').read() != open(origen, 'rb').read():
                    shutil.copyfile(origen, copia)
                    escritos.append(rel)
                continue
            texto = open(origen, encoding='utf-8', newline='').read()
            nuevo = traducir_texto(texto, seguras, faltan)
            for vieja, nueva in huellas:
                nuevo = re.sub(vieja, nueva, nuevo, flags=re.IGNORECASE)
            cambios += nuevo != texto
            escribir(rel, nuevo)

    # the rest of app/ that the build uses
    for rel in ('CMakeLists.txt', 'CMakePresets.json', 'nfsmw.toml', 'orden_funciones.ld', 'overrides.toml',
                os.path.join('generated', 'rexglue.cmake')):
        if not os.path.exists(os.path.join(APP, rel)):
            # rexglue.cmake only exists after a codegen of app/; the codegen of the edition writes its own
            continue
        texto = open(os.path.join(APP, rel), encoding='utf-8', newline='').read()
        if rel == 'CMakeLists.txt':
            # PGO profile translated to this edition (tools/editions/pgo/traducir_perfil.py)
            if texto.count('/../pgo/pal_es"') != 1:
                raise SystemExit('CMakeLists.txt: profile folder not found')
            texto = texto.replace('/../pgo/pal_es"', '/../pgo/%s"' % edicion)
        if rel == 'orden_funciones.ld':
            # it only orders hot functions; a doubtful entry does no harm (the linker ignores a name that
            # does not exist), so the doubtful translation is acceptable too
            escribir(rel, traducir_texto(texto, {**todas, **seguras}, set()))
            continue
        escribir(rel, traducir_texto(texto, seguras, faltan))

    # gaps: a function declaration at a doubtful place can split another function; those are removed and
    # tools/huecos.py will find them after the codegen
    quitados = []
    lineas = []
    for linea in open(os.path.join(APP, 'huecos.toml'), encoding='utf-8', newline='').read().splitlines(True):
        m = re.match(r'\s*"0x(82[0-9A-Fa-f]{6})"', linea)
        if m and int(m.group(1), 16) not in seguras:
            # {"huecos": {PAL: other}} in --parejas: the equivalent gap in the other edition, found by hand
            # with tools/huecos.py (same size, right after the equivalent function). It keeps its options.
            otra = correcciones.get('huecos', {}).get(m.group(1).upper())
            if otra:
                lineas.append(linea.replace(m.group(1), otra))
                continue
            quitados.append(m.group(1))
            continue
        lineas.append(traducir_texto(linea, seguras, faltan) if not m else
                      traducir_texto(linea, {**todas, **seguras}, set()))
    escribir('huecos.toml', ''.join(lineas))

    manifiesto = open(os.path.join(APP, 'nfsmw_manifest.toml'), encoding='utf-8', newline='').read()
    manifiesto = manifiesto.replace('file_path = "../assets/game_root/default.xex"', 'file_path = "%s"' % xex)
    # the codegen requires the XEX to be inside game_root
    manifiesto = manifiesto.replace('game_root = "../assets/game_root"', 'game_root = "%s"' % os.path.dirname(xex))
    escribir('nfsmw_manifest.toml', manifiesto)

    # function partition seeded with the Spanish one. First the safe and the corrected pairs; a doubtful one
    # only if its slot is still free (a misplaced doubtful one can land on top of another function's exact pair).
    fiables = dict(seguras)
    fiables.update({int(p, 16): int(o, 16) for p, o in correcciones.get('parejas', {}).items()})
    sembrado = {}
    for mapa in (fiables, todas):
        for direccion, fichero in reparto['assignments'].items():
            d = mapa.get(int(direccion, 16))
            if d is not None and '%08X' % d not in sembrado:
                sembrado['%08X' % d] = fichero
    sembrado.update(correcciones.get('nuevas', {}))
    reparto['assignments'] = dict(sorted(sembrado.items()))
    escribir(os.path.join('generated', 'default', 'codegen.partition.json'), json.dumps(reparto, indent=2) + '\n')

    # weak calls, for tools/llamadas_directas.py: the functions that the tested build (app/generated/default)
    # calls through the weak path, translated. That way every call comes out the same as in the Spanish
    # edition, call by call. A list taken from the current sources does not work: they mention new addresses
    # since the last run of the tool.
    enganchadas = set()
    for x in debiles:
        d = todas.get(int(x, 16))
        enganchadas.add('sub_%08X' % (d if d is not None else int(x, 16)))
    escribir('enganchadas.txt', '\n'.join(sorted(enganchadas)) + '\n')

    print('ficheros reescritos: %s' % (', '.join(escritos) or 'ninguno'))
    print('%s: %d fuentes con cambios; %d funciones sembradas; %d huecos quitados %s; %d con gancho' % (
        destino, cambios, len(sembrado), len(quitados), quitados, len(enganchadas)))
    if faltan:
        print('SIN TRADUCCION SEGURA (revisar a mano): %s' % ', '.join(sorted(faltan)))
        sys.exit(1)


if __name__ == '__main__':
    main()
