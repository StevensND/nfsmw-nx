#pragma once

#include "shader.h"
#include "shader_code.h"

struct StringBuffer
{
    std::string out;

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }
};

struct ShaderRecompiler : StringBuffer
{
    uint32_t indentation = 0;
    bool isPixelShader = false;
    const uint8_t* constantTableData = nullptr;
    std::unordered_map<uint32_t, VertexElement> vertexElements;
    std::unordered_map<uint32_t, std::string> interpolators;
    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants;
    std::unordered_map<uint32_t, const char*> boolConstants;
    std::unordered_map<uint32_t, const char*> samplers;
    std::unordered_map<uint32_t, uint32_t> ifEndLabels;
    uint32_t specConstantsMask = 0;

#ifdef UNLEASHED_RECOMP
    bool hasMtxProjection = false;
    bool hasMtxPrevInvViewProjection = false;
#endif

    void indent()
    {
        for (uint32_t i = 0; i < indentation; i++)
            out += '\t';
    }

    /*
     * Merging predicated blocks.
     *
     * The translator emitted an `if (p0) { ... }` for every predicated instruction, nested
     * inside the `if (p0)` of the EXEC block as well. In p_000101 (the ground with shadow)
     * that was 75 OpBranchConditional, 206 OpLabel and 49 OpPhi for 10 texture samples and
     * 49 scalar operations. The worst part is not the cost of the branch: the 9 PCF taps end
     * up in 9 different basic blocks, so the scheduler cannot issue them together and the
     * texture latency is paid nine times instead of once. The scene runs at 31 % of peak
     * arithmetic throughput.
     *
     * With this, consecutive predicated instructions with the same predicate share a single
     * block. The semantics are identical (not a single operation is touched), except for the
     * rule that forces a close:
     *
     *   1. before an unpredicated instruction,
     *   2. after any instruction that writes p0 (otherwise the next one would run testing
     *      the old p0: that would change the result),
     *   3. at every control flow boundary (EXEC, jump, loop, return).
     *
     * predAbierto_: -1 none, 0 open with !p0, 1 open with p0.
     */
    int predAbierto_ = -1;

    /*
     * The close is lazy, so merging also works across EXEC blocks.
     *
     * The first version of the merge only joined instructions inside the same EXEC block,
     * because every control flow boundary forces a close (rule 3). But the Xenos splits
     * blocks every few instructions on its own (the EXEC cannot hold more), and the result
     * was that the nine PCF samples still fell into three basic blocks:
     *
     *     if (p0) { ...tap 1... }
     *     if (p0) { ...taps 2-7... }     <- three EXECs in a row, same p0, nothing in between
     *     if (p0) { ...taps 8-9... }
     *
     * Between the `}` and the next `if` nothing is emitted: no conditional EXEC, no jump,
     * no loop, no p0 write. So the `}` is redundant and so is the `if` after it.
     *
     * So the close is not decided on the spot: it is written, and if the next thing to be
     * emitted opens again with the same condition and nothing has been written in between,
     * the `}` is deleted and emission continues inside the same block. The check is on the
     * text (`finCierre_ == out.size()`), which is exactly the condition needed: anything that
     * could change p0 (or put control flow in between) emits text, and then there is no
     * merge. The only exception is the close caused by a p0 write (the text was emitted
     * before the `}`), and that one is marked by hand as not cancelable.
     *
     * 185 of the 298 predicated blocks of the 109 pixel shaders merge this way (113 remain),
     * and 20 of the 36 in the vertex shaders. The nine shadow map taps end up in a single
     * basic block, which is what the scheduler needed to issue them together.
     *
     * Almost none of those blocks are opened by abrirPredicado(): they are opened by the
     * control flow's own CondExecPred, which in this game comes in runs of three or four in
     * a row with the same predicate. That is why the close that must be undoable is also
     * the CondExecPred one, and why recompile() records its brace with anotarCierreCf().
     * There is one more condition there that is not needed here: if some instruction in the
     * block wrote p0, the next block has to test the new p0 and merging would leave it with
     * the old one. That shows in predGarantizado_, which cerrarSiEscribePredicado() sets to
     * -1 right when it happens.
     */
    size_t marcaCierre_ = std::string::npos;  // where the just-written "}" starts (with its tabs)
    size_t finCierre_ = std::string::npos;    // where it ends; only valid while it is still out.size()
    int cierreCondicion_ = -1;                // the condition that block was open with

    /*
     * If the last thing written is the `}` of a predicate block with this condition and there is
     * nothing after it, it is deleted and we are back inside. The caller decides who owns the
     * brace that will close it: predAbierto_ (abrirPredicado) or the control flow (CondExecPred).
     */
    bool reabrirCierre(int quiero)
    {
        if (predAbierto_ >= 0 || cierreCondicion_ != quiero)
            return false;
        if (finCierre_ == std::string::npos || finCierre_ != out.size())
            return false;
        out.resize(marcaCierre_);
        ++indentation;
        marcaCierre_ = finCierre_ = std::string::npos;
        return true;
    }

    /*
     * The brace with which the control flow closes a CondExecPred `if (p0)`. It is recorded as
     * undoable only if the block does not end in `return` and nothing wrote p0 inside.
     */
    void anotarCierreCf(size_t marcaLlave, int condicion)
    {
        if (condicion >= 0 && predGarantizado_ == condicion)
        {
            marcaCierre_ = marcaLlave;
            finCierre_ = out.size();
            cierreCondicion_ = condicion;
        }
        else
        {
            marcaCierre_ = finCierre_ = std::string::npos;
        }
    }

    /*
     * The second `if (p0)`, the redundant one.
     *
     * An EXEC conditioned on the predicate (CondExecPred) opens its own `if (p0)` around the
     * whole block. Inside, each predicated instruction opened another identical one:
     *
     *     if (p0) { if (p0) { ...the nine PCF taps... } }
     *
     * That was 307 of the 641 `if (p0)` left after the first merge pass. The inner one decides
     * nothing: we are already inside the outer one.
     *
     * predGarantizado_ says the control flow already guarantees that predicate, so
     * abrirPredicado() emits nothing. And it is invalidated as soon as something writes p0:
     * from then on the guaranteed value is the old one and it has to be really tested again.
     * That is the only rule that can break this, and it is the same one as for block merging.
     */
    int predGarantizado_ = -1;

    void abrirPredicado(bool condicion)
    {
        const int quiero = condicion ? 1 : 0;
        if (predAbierto_ == quiero)
            return;  // already inside the right block: emit nothing
        if (predAbierto_ < 0 && predGarantizado_ == quiero)
            return;  // the EXEC already guarantees it from outside: the inner `if` was redundant
        // The last thing written is the `}` of a block with this same condition and nothing came
        // after it: the close is deleted and emission continues inside. See marcaCierre_.
        if (reabrirCierre(quiero))
        {
            predAbierto_ = quiero;  // now cerrarPredicado() closes the brace
            return;
        }
        cerrarPredicado();
        indent();
        println("if ({}p0)", condicion ? "" : "!");
        indent();
        out += "{\n";
        ++indentation;
        predAbierto_ = quiero;
    }

    void cerrarPredicado()
    {
        if (predAbierto_ < 0)
            return;
        --indentation;
        const size_t marca = out.size();
        indent();
        out += "}\n";
        // Recorded in case the next instruction asks for the same predicate again.
        marcaCierre_ = marca;
        finCierre_ = out.size();
        cierreCondicion_ = predAbierto_;
        predAbierto_ = -1;
    }

    /*
     * Closes if the text emitted since `marca` writes the predicate. The text is checked rather
     * than the opcode on purpose: both places that write p0 (SetpEqPush..SetpGePush for vector
     * and SetpEq..SetpRstr for scalar) emit "p0 = ", and searching for it cannot go stale if a
     * third one is ever added.
     */
    void cerrarSiEscribePredicado(size_t marca)
    {
        if (out.find("p0 = ", marca) == std::string::npos)
            return;
        cerrarPredicado();
        // The predicate the EXEC guaranteed is no longer valid: it was just rewritten.
        predGarantizado_ = -1;
        /*
         * And this close cannot be undone. It is the only case in which the text that changes p0
         * was emitted before the `}`, so the `finCierre_ == out.size()` rule does not see it:
         * merging here would put the next instruction in the block of the old p0.
         */
        marcaCierre_ = finCierre_ = std::string::npos;
    }

    void printDstSwizzle(uint32_t dstSwizzle, bool operand);
    void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle);

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, bool bicubic);
    void recompile(const AluInstruction& instr);

    void recompile(const uint8_t* shaderData, const std::string_view& include);

#ifdef NFSMW_RECOMP
    // The #else of the constants block under NFSMW_DEKO3D: the float constants from the UBOs only, and each
    // sampler register as a texture and a sampler at fixed bindings (shader_common.h). Text only: the maps
    // above are filled by the Vulkan block.
    void printDeko3dConstants(const ConstantTableContainer* constantTableContainer);
#endif
};
