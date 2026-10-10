#!/bin/bash
# nfsmw_regenerar_biblioteca_pcf.sh SALIDA ENTRADA
# Regenerates the NFSSPV library from bash with the same options as nfsmw_validar.ps1
# (which on Windows PowerShell 5.1 stops at the first DXC warning):
#   1. builds the translator with -DNFSMW_RECOMP and the format regression test;
#   2. translates nfsmw_shaders_combinados into SALIDA/hlsl;
#   3. DXC, spirv-val, packaging and library regression (nfsmw_completar_spirv.sh).
# SALIDA must be a new folder. ENTRADA is the folder with the game's shader containers.
set -u
# Tools: g++ (MinGW on Windows), DXC and spirv-val from the Vulkan SDK, and Python ("py" on Windows).
# SDK is the sdk/ folder of this repository with its thirdparty sources; MESA is the mesa-switch source tree
# (its src/util/xxhash.h is the one the translator was built with).
RAIZ=$(cd "$(dirname "$0")" && pwd)
SDK=${SDK:-$RAIZ/../sdk}
MESA=${MESA:?set MESA to the mesa-switch source tree}
SALIDA=${1:?falta la carpeta de salida}
ENTRADA=${2:?falta la carpeta de entrada}
DXC=${DXC:-dxc}
VAL=${SPIRV_VAL:-spirv-val}

[ -e "$SALIDA" ] && { echo "La carpeta de salida ya existe"; exit 1; }
mkdir -p "$SALIDA/hlsl" "$SALIDA/spirv"

g++ -std=c++23 -O1 "-I$RAIZ" "-I$RAIZ/XenosRecomp" "-I$SDK/thirdparty/fmt/include" \
  "-I$MESA/src/util" -include "$RAIZ/pch_min.h" \
  -DFMT_HEADER_ONLY -DXXH_INLINE_ALL -DNFSMW_RECOMP \
  "$RAIZ/nfsmw_hlsl.cpp" "$RAIZ/XenosRecomp/shader_recompiler.cpp" -o "$RAIZ/nfsmw_hlsl.exe" \
  > "$SALIDA/compilar.log" 2>&1 || { echo "Fallo al compilar el traductor; ver compilar.log"; exit 1; }
# Format regression against a reference set of containers, when there is one (REFERENCIA).
if [ -n "${REFERENCIA:-}" ]; then
  "$RAIZ/nfsmw_probar_contenedor.exe" "$ENTRADA" "$REFERENCIA" > "$SALIDA/formato.log" \
    || { echo "Fallo de regresion del formato; ver formato.log"; exit 1; }
fi
"$RAIZ/nfsmw_hlsl.exe" "$ENTRADA" "$SALIDA/hlsl" "$RAIZ/XenosRecomp/shader_common.h" > "$SALIDA/traduccion.log" \
  || { echo "Hay shaders sin traducir; ver traduccion.log"; exit 1; }

# The translator emits tfetch2D for every sample; here only the ones that use SHADOWMAP_SAMPLER, the
# shadow map samples, are changed to tfetch2DSombra. That function obeys SPEC_CONSTANT_PCF_BARATO:
# with the bit set, the nine samples of the 3x3 land on the same texel and the compiler keeps one.
# Without the bit, the generated code is identical to the unmodified one.
cambios=$(grep -l "tfetch2D(SHADOWMAP_SAMPLER_" "$SALIDA"/hlsl/*.hlsl 2>/dev/null | wc -l)
llamadas=$(grep -ho "tfetch2D(SHADOWMAP_SAMPLER_" "$SALIDA"/hlsl/*.hlsl 2>/dev/null | wc -l)
sed -i 's/tfetch2D(SHADOWMAP_SAMPLER_/tfetch2DSombra(SHADOWMAP_SAMPLER_/g' "$SALIDA"/hlsl/*.hlsl
echo "PCF barato: $llamadas llamadas al mapa de sombras en $cambios shaders"
[ "$llamadas" -gt 0 ] || { echo "No se reescribio ninguna llamada: algo va mal"; exit 1; }

# Shadow by minimum (nfsmw_nativo_sombra_minimo; SPEC_CONSTANT_SOMBRA_MINIMO in shader_common.h).
# Shadow map calls become tfetch2DSombraMin with the 3D index of the same register, which is where the
# app puts the second texture. Without the bit the code is that of tfetch2DSombra. All of them must be
# converted, or no library is produced.
cat > "$SALIDA/sombra_minimo.py" <<'FIN_SOMBRA_MINIMO'
import glob, io, os, sys
cod = chr(117) + chr(116) + chr(102) + chr(45) + chr(56)
corto = 'tfetch2DSombra(SHADOWMAP_SAMPLER_'
viejo = 'tfetch2DSombra(SHADOWMAP_SAMPLER_Texture2DDescriptorIndex, SHADOWMAP_SAMPLER_SamplerDescriptorIndex,'
nuevo = ('tfetch2DSombraMin(SHADOWMAP_SAMPLER_Texture2DDescriptorIndex, SHADOWMAP_SAMPLER_Texture3DDescriptorIndex, '
         'SHADOWMAP_SAMPLER_SamplerDescriptorIndex,')
define3d = '#define SHADOWMAP_SAMPLER_Texture3DDescriptorIndex'
shaders = llamadas = 0
for f in sorted(glob.glob(os.path.join(sys.argv[1], '*.hlsl'))):
    s = io.open(f, encoding=cod, newline='').read()
    n = s.count(corto)
    if not n:
        continue
    if s.count(viejo) != n or define3d not in s:
        print('sombra por minimo: %s tiene %d llamadas y %d con la forma esperada' % (os.path.basename(f), n, s.count(viejo)))
        sys.exit(1)
    io.open(f, 'w', encoding=cod, newline='').write(s.replace(viejo, nuevo))
    shaders += 1
    llamadas += n
print('sombra por minimo: %d llamadas en %d shaders' % (llamadas, shaders))
sys.exit(0 if llamadas else 1)
FIN_SOMBRA_MINIMO
py "$SALIDA/sombra_minimo.py" "$SALIDA/hlsl" || { echo "No se reescribieron las llamadas del mapa de sombras"; exit 1; }

# The radial blur of the final composition (p_000139) behind a specialization constant. The factor
# lives in r0.x and is used in `r5 * r0.xxx + r3`; with r0.x = 0 the output is the center tap and the
# seven offset taps plus the HEIGHTMAP one are dead.
comp=$SALIDA/hlsl/p_000139.hlsl
if [ -f "$comp" ]; then
  # Careful: setting the factor to 0 does not work. In floating point x*0 is not folded (NaN/Inf), so
  # the chain of the seven taps survives DCE. The blend itself has to be cut: with the bit set the
  # output is directly the center tap r3, and then r5 is unused and the taps die.
  py -c "
import io,sys
R=sys.argv[1]
s=io.open(R,encoding=chr(117)+chr(116)+chr(102)+chr(45)+chr(56)).read()
v='r5.xyz = r5.xyz * r0.xxx + r3.xyz;'
n=s.count(v)
if n!=1:
    print('la mezcla del desenfoque aparece %d veces, esperaba 1' % n); sys.exit(1)
s=s.replace(v,'r5.xyz = (g_SpecConstants() & SPEC_CONSTANT_SIN_DESENFOQUE) ? r3.xyz : (r5.xyz * r0.xxx + r3.xyz);',1)
io.open(R,'w',encoding=chr(117)+chr(116)+chr(102)+chr(45)+chr(56)).write(s)
print('desenfoque de la composicion: mezcla reescrita')
" "$comp" || { echo "No se reescribio la mezcla del desenfoque"; exit 1; }
fi

: > "$SALIDA/dxc.log"
: > "$SALIDA/spirv-val.log"
n=0
for f in "$SALIDA"/hlsl/*.hlsl; do
  base=$(basename "$f" .hlsl)
  if [[ "$base" == p_* ]]; then tipo=ps_6_6; extra=(); else tipo=vs_6_6; extra=(-fvk-invert-y); fi
  "$DXC" -spirv -T "$tipo" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
    -Werror=parameter-usage "${extra[@]}" -Fo "$SALIDA/spirv/$base.spv" "$SALIDA/hlsl/$base.hlsl" \
    2>> "$SALIDA/dxc.log" || { echo "DXC rechazo $base"; exit 1; }
  "$VAL" --target-env vulkan1.2 --scalar-block-layout "$SALIDA/spirv/$base.spv" \
    2>> "$SALIDA/spirv-val.log" || { echo "SPIR-V invalido: $base"; exit 1; }
  n=$((n + 1))
done
esperados=$(ls "$ENTRADA"/*.bin | wc -l)
echo "spirv=$n esperados=$esperados"
[ "$n" -eq "$esperados" ] || { echo "El numero de salidas no coincide con la entrada"; exit 1; }
# The tfetch2DSombraMin marker (NFSMW_MARCA_SOMBRA_MINIMO, an OpConstant) must be in the SPIR-V of
# every pixel shader that uses it: it is what the app checks to know it can ask for the minimum.
cat > "$SALIDA/marca_sombra_minimo.py" <<'FIN_MARCA_SOMBRA'
import glob, io, os, struct, sys
cod = chr(117) + chr(116) + chr(102) + chr(45) + chr(56)
def tiene_marca(ruta):
    b = open(ruta, 'rb').read()
    w = struct.unpack('<%dI' % (len(b) // 4), b[:len(b) // 4 * 4])
    if len(w) < 5 or w[0] != 0x07230203:
        return False
    i = 5
    while i < len(w):
        n = w[i] >> 16
        if n == 0 or i + n > len(w):
            return False
        if (w[i] & 0xFFFF) == 43 and n == 4 and w[i + 3] == 0x5E3B1A84:
            return True
        i += n
    return False
con = sin = 0
for f in sorted(glob.glob(os.path.join(sys.argv[1], 'hlsl', 'p_*.hlsl'))):
    if 'tfetch2DSombraMin(SHADOWMAP_SAMPLER_' not in io.open(f, encoding=cod).read():
        continue
    spv = os.path.join(sys.argv[1], 'spirv', os.path.basename(f)[:-5] + '.spv')
    if os.path.exists(spv) and tiene_marca(spv):
        con += 1
    else:
        sin += 1
        print('sin la marca de tfetch2DSombraMin: ' + os.path.basename(spv))
print('marca de tfetch2DSombraMin: %d pixel shaders con ella y %d sin ella' % (con, sin))
sys.exit(0 if con and not sin else 1)
FIN_MARCA_SOMBRA
py "$SALIDA/marca_sombra_minimo.py" "$SALIDA" || { echo "Falta la marca de tfetch2DSombraMin en el SPIR-V"; exit 1; }
"$RAIZ/nfsmw_empaquetar.exe" "$ENTRADA" "$SALIDA/spirv" "$SALIDA/nfsmw_shaders.nfsp" > "$SALIDA/paquete.log" \
  || { echo "Fallo al empaquetar; ver paquete.log"; exit 1; }
"$RAIZ/nfsmw_probar_biblioteca.exe" "$SALIDA/nfsmw_shaders.nfsp" "$ENTRADA" > "$SALIDA/biblioteca.log" \
  || { echo "Fallo de regresion de biblioteca; ver biblioteca.log"; exit 1; }
ls -la "$SALIDA/nfsmw_shaders.nfsp"

# DEKO3D=1: also the deko3d library (docs/HowToDeko3d.md, section 3): the same rewritten HLSL compiled with
# -DNFSMW_DEKO3D and without -fvk-invert-y (deko3d's clip-space y points up, as in Direct3D), packed the same way.
# DKSH=path/to/nfsmw_dksh (shaders/dksh) then compiles it to a DKSH cache and checks every variant.
if [ "${DEKO3D:-0}" = 1 ]; then
  mkdir -p "$SALIDA/spirv_dk"
  : > "$SALIDA/dxc_dk.log"
  for f in "$SALIDA"/hlsl/*.hlsl; do
    base=$(basename "$f" .hlsl)
    if [[ "$base" == p_* ]]; then tipo=ps_6_6; else tipo=vs_6_6; fi
    "$DXC" -spirv -T "$tipo" -E main -HV 2021 -fspv-target-env=vulkan1.2 -fvk-use-dx-layout \
      -Werror=parameter-usage -DNFSMW_DEKO3D -Fo "$SALIDA/spirv_dk/$base.spv" "$f" \
      2>> "$SALIDA/dxc_dk.log" || { echo "DXC rechazo $base para deko3d"; exit 1; }
  done
  "$RAIZ/nfsmw_empaquetar.exe" "$ENTRADA" "$SALIDA/spirv_dk" "$SALIDA/nfsmw_shaders_dk.nfsp" > "$SALIDA/paquete_dk.log" \
    || { echo "Fallo al empaquetar la biblioteca de deko3d; ver paquete_dk.log"; exit 1; }
  ls -la "$SALIDA/nfsmw_shaders_dk.nfsp"
  if [ -n "${DKSH:-}" ]; then
    "$DKSH" "$SALIDA/nfsmw_shaders_dk.nfsp" "$SALIDA/shadercache_dksh.bin" --contenedores "$ENTRADA" \
      --informe "$SALIDA/dksh.tsv" > "$SALIDA/dksh.log" || { echo "Hay variantes sin DKSH; ver dksh.log"; exit 1; }
    tail -7 "$SALIDA/dksh.log"
  fi
fi
echo "regeneracion completada"
