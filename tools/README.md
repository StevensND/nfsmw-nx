# Tools

This folder has the scripts that build the port: they get the missing libraries, extract the game, translate it to
C++, fix up the generated code, check the result and build the NRO. Look here when a step of
[docs/building.md](../docs/building.md) names a script and you want to know what it does. The names are in Spanish;
each one is translated below.

## Main steps

In the order of [docs/building.md](../docs/building.md):

| File | What it does |
|---|---|
| `fetch_thirdparty.py` | Fetches the third-party sources of the SDK into `sdk/thirdparty`. The repository only keeps the files this port changed. |
| `fase1_extraer.py` ("phase 1: extract") | Extracts the Xbox 360 ISO into `assets/game_root` and prints the [XEX](../docs/glossary.md#xex) information. |
| `codegen.sh` | Runs the code generator (`rexglue codegen`, which translates the game to C++) on an edition tree (`app/`, or the `app_<edition>` copy of another [edition](../docs/glossary.md#edition)), then `llamadas_directas.py` and `copia_literal.py`. |
| `build.ps1` | Configures and builds the [NRO](../docs/glossary.md#nro) of the default tree (`app/`). |

## After every code generation

The build depends on these two tools. `codegen.sh` runs them for you, right after the code generator. If you run the
code generator another way, run them afterwards, in this order.

| File | What it does |
|---|---|
| `llamadas_directas.py` ("direct calls") | Turns calls between recompiled functions into direct calls, except for calls to functions with a [hook](../docs/glossary.md#hook). [LTO](../docs/glossary.md#lto) needs this to optimise across them (see [docs/toolchain.md](../docs/toolchain.md#direct-calls)). |
| `copia_literal.py` ("literal copy") | Writes the literal copies of five game functions. The [guards](../docs/glossary.md#guard) of their [native versions](../docs/glossary.md#native-replacement) compare against these copies. |

## Checks and analysis

| File | What it does |
|---|---|
| `huecos.py`, `huecos_excluir.txt` ("gaps", "gaps to exclude") | Finds code the code generator's analysis left without a function and writes it to `app/huecos.toml`. `huecos_excluir.txt` lists the gaps to leave out. |
| `lee_antes.py` ("read before") | Lists, per recompiled function, the non-volatile registers (the ones a function must keep unchanged for its caller) it reads before writing them. See [docs/toolchain.md](../docs/toolchain.md#registers-as-c-locals) for why this matters. |
| `calientes.py` ("hot ones") | Resolves the samples of the console profiler (`rex_perfil.log`) to functions, hottest first. Its output is the input for `app/orden_funciones.ld`, the list of functions placed first in the program ([function ordering](../docs/glossary.md#function-ordering)). |
| `comprobar_clave_textura.py` ("check texture key") | Checks in the built ELF (the program before it is packed as an NRO) that the texture key is written before it is hashed. It catches an LTO/PGO aliasing pitfall (see [docs/toolchain.md](../docs/toolchain.md#an-lto-and-pgo-pitfall-strict-aliasing-in-xxhash)). |

## Folders

| Folder | What it has |
|---|---|
| `switch/cmake/` | The CMake toolchain for devkitA64 and [libnx](../docs/glossary.md#libnx), `switch-devkitA64.cmake`: the file that tells CMake how to compile for the Switch. |
| `editions/` | Support for the other editions: address matching, tree creation, checks, profile translation, per-edition build ([docs/editions.md](../docs/editions.md)). |
| `french-dub/` | Makes the PAL (English) edition French (text, movies, in-game speech) from the player's own original Xbox French disc ([french-dub/README.md](french-dub/README.md)). |
