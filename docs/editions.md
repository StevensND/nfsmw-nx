# Supporting other editions and languages

The game was sold in several [editions](glossary.md#edition): versions for different countries and languages. Each
edition has its own `default.xex` (the game's program, see [XEX](glossary.md#xex)), and every different `default.xex`
is a different program that needs its own build of the NRO. This page lists the editions, explains how the port's
code is carried from one edition to another, and how the installer page picks the right build. Read it to add an
edition, or when you port a game that came out in several editions.

The [installer page](https://stevensnd.github.io/nfsmw-nx-installer/) picks the build by the SHA-256 (a fingerprint
of a file's contents) of the player's executable. For editions that were verified with a specific disc, it also
checks the size of one data file, `NFS/ZZDATA0.BIN`. See [Installer manifest](#installer-manifest).

## Editions

The PAL Spanish edition is the *reference*: all the port's code is written for it. Three words help to read the
table:

- A *compilation* is one build of the game's code, made by its developers. Editions from the same compilation have
  the same code at the same addresses.
- `.text` is the part of the program that holds the code, `.rdata` holds constant data such as strings, and `.data`
  holds the game's variables.
- The *profile* is the [PGO](glossary.md#pgo) profile the build is optimised with (see step 6 below).

| Edition | Relation to the reference (PAL Spanish) |
|---|---|
| PAL Spanish | The reference edition. Every address in the sources refers to it. A community Brazilian Portuguese translation uses it too: it changes some data files and the videos, but not the shaders. |
| PAL French, PAL German, PAL Italian | The same compilation of the game ("NfsMWEurope...Release"). Only five instructions that load the language constant and some strings in `.rdata` differ. Same addresses, same profile, only five generated files differ. |
| PAL English | A different compilation, close to the US one. A community Russian translation uses it too, and only replaces `NFS/ZZDATA0.BIN`. |
| PAL Polish (fan translation) | Made on the PAL English edition, with its own build. Its `default.xex` was rebuilt without encryption or compression (the installer reads that kind of executable too), and one branch is patched: at 0x8228753C, in PAL English addresses, a `beq` became a `nop`. Same addresses and profile as PAL English; only one generated file differs. |
| USA | A different compilation: `.text`, `.rdata` and the `.embsec_` sections (data embedded in the executable) move. |
| Japan | A different compilation where **`.data` moves too**, piece by piece (mostly by +0x5A0/+0x5A4). |

Other PAL languages are probably the same compilation as the Spanish, French, German and Italian ones, with a
different language constant.

### Shaders that change between editions

The renderer recognises some [shaders](glossary.md#shader) by their hash (a fingerprint of their bytes). Some of
those hashes are different in some editions: for example, the glow and sky shaders of the US and Japanese builds.
The final composition shader is also stored in a container (a file inside the game data that holds shaders) with a
different name in each edition. So the renderer and the installer find it by its content, not by its name.

## Porting the hooks to another edition

Every [hook](glossary.md#hook), every override (the functions declared by hand for the code generator, in
`app/overrides.toml`) and every function boundary (where a function starts and ends) is written with the addresses
of the reference executable. In other editions the same functions are often at other addresses. The tools in
[`tools/editions/`](../tools/editions) translate them, in this order:

1. **Match the two executables.** `emparejar.py` ("match") finds where each address of the reference is in the other
   edition. It aligns the code of both executables using runs of 12 [PowerPC](glossary.md#powerpc) instructions
   (12-grams) as anchors. The instructions are normalised first, which means the parts that depend on where the code
   sits (such as branch offsets) are removed. Only anchors that come in a consistent order are kept (a longest
   increasing subsequence). `.rdata` is matched by content, with windows of 32 to 1,024 bytes. It translates the
   addresses it is given, as arguments or one per line on standard input, and writes a table (`.tsv`) with the name
   you give as the third argument. The addresses to give it are all the ones the reference tree uses, and
   `direcciones.py` ("addresses") lists them: the sources, the code generator settings, the linker order, the
   function partition and the weak calls of the generated code. The partition and the generated code only exist once
   the code generator has run on the reference tree, so run it first:

   ```bash
   python tools/editions/direcciones.py app > addresses.txt
   python tools/editions/emparejar.py <PAL image> <other image> todas.tsv < addresses.txt
   ```

   The images are the decompressed executables (the PE image inside each `default.xex`), not the `.xex` files.
   `test/save_image.mjs` in the [installer's repository](https://github.com/StevensND/nfsmw-nx-installer) saves it
   from an ISO (`node test/save_image.mjs <iso> <output image>`).
2. **Verify the hooked functions.** `verificar_ganchos.py` ("verify hooks") compares every hooked function in full
   between the two executables. This is needed because step 1 only compares short pieces of code, and a hook depends
   on the whole function being the same.
3. **Translate `.data` references.** `.data` normally stays at the same address, but in the Japanese edition it moves
   in pieces. PowerPC code builds a full address with a pair of instructions: `lis` loads the high part, and a second
   instruction supplies the low part. `datos_por_referencias.py` ("data by references") follows those pairs in the
   code to find where each referenced piece of data went. `verificar_parejas.py` ("verify pairs") then checks every
   pair strictly (856 pairs for the Japanese edition, all consistent).
4. **Create the edition tree.** An edition tree, `app_<edition>`, is a copy of the application with every address
   translated. Create it with `crear_arbol.py <edition> <map> <xex> --parejas <json>` ("create tree"). `--parejas`
   gives pairs fixed by hand (the ones used are in the `parejas_corregidas.json` files under `tools/editions/`). The
   script also does three more things:
   - It seeds the function partition of the code generator (how functions are split across the generated files) with
     the reference one, exact pairs first.
   - It copies the list of weak callees from the reference's generated code: the functions it still calls through
     their weak alias (see [Direct calls](toolchain.md#direct-calls) in toolchain.md). So every call comes out the
     same.
   - It translates names that are split in two and joined by the C preprocessor (token pasting), for example
     `UNIR_(__imp__sub_824F, D7C0)`. A plain search for addresses does not see them.
5. **Generate the code** for the edition. The code generator skips its work when its inputs have not changed, and it
   keeps track of them in `codegen.stamp` (in the tree's `generated/default` folder). If only the partition changed,
   delete `codegen.stamp` to force a new generation. Then run the steps that follow every code generation:
   `llamadas_directas.py` with the list of hooked addresses of that edition (see [toolchain.md](toolchain.md)).
   `tools/codegen.sh` does both for an edition tree; its usage is at the top of the script. To check the result,
   `comparar_reparto.py` compares the partition with the reference, and `tools/huecos.py` shows the gaps (code left
   without a function) that need pairs fixed by hand. The Japanese edition needed six.
6. **Translate the PGO profile.** `tools/editions/pgo/traducir_perfil.py` renames each function's counters through
   the address map. So the other edition gets the same optimisation without playing it again (see
   [toolchain.md](toolchain.md#pgo-with-gcc-16-on-horizon)). The translated profiles are in `pgo/<edition>/`.
7. **Build from the reference paths.** In a GCC profile, the identifier of a local function (one that only exists
   inside one file) includes the path of its source file. So the edition tree must be built from the same paths as
   the reference. The build script, `tools/editions/build_edition.ps1`, swaps the edition tree into the place of
   `app/` for the duration of the build, and swaps it back at the end:

   ```powershell
   powershell -ExecutionPolicy Bypass -File tools\editions\build_edition.ps1 -Edition usa
   ```

Before shipping an edition build, run the same checks as for the reference (see [toolchain.md](toolchain.md)):
`_start` at address 0, the texture key check (`tools/comprobar_clave_textura.py`), and the same number of installed
hooks as the reference.

## Installer manifest

The installer page reads `release/manifest.json`, in the
[installer's repository](https://github.com/StevensND/nfsmw-nx-installer). Each entry gives:

- the edition name and the SHA-256 of its executable;
- the NRO to use and its SHA-256;
- the expected SHA-256 and size of the shader library built from that disc;
- the SHA-256 of the final composition shader container, which the page uses to find that shader by content.

An entry can also name a disc file and its size. Then a known executable on an untested disc (a reprint or a
translation) is reported, instead of producing a build that nobody has verified.
