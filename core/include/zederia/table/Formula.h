// The formula language of the wavetable workshop (plan 3.10, Appendix B): one expression that
// generates frames in the wave or the harmonic domain. Expressions compile to a small bytecode
// with a size limit; there are no loops, variables or side effects, so evaluation always ends.
//
//   Symbols    t (cycle position 0..1), x (-1..1), y (frame position 0..1), z (-1..1),
//              h (harmonic number, harmonics domain), in (existing value), sel (value of the
//              selected frame), rand (seeded noise -1..1, fixed per position), pi, tau, e
//   Operators  + - * / ^ %   < <= > >= == !=   && || !   cond ? a : b
//   Functions  sin cos tan asin acos atan sinh cosh tanh exp ln log2 log10 sqrt abs sign floor
//              ceil round frac min max clamp mix pow mod step smoothstep saw square tri noise
#pragma once

#include "zederia/table/Toolkit.h"
#include "zederia/table/Wavetable.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zed {

struct FormulaVars {
    double t = 0.0, x = 0.0, y = 0.0, z = 0.0, h = 0.0, in = 0.0, sel = 0.0, rand = 0.0;
    std::uint64_t seed = 1; // not a symbol: what noise() is seeded with
};

struct FormulaError {
    int position = 0;    // 0-based character position in the text
    std::string message; // what is wrong, in plain words
    // "column 12: unknown function "sine"; did you mean "sin"?"
    std::string describe() const { return "column " + std::to_string(position + 1) + ": " + message; }
};

class Formula {
public:
    static constexpr int kMaxNodes = 512;   // parts an expression may have
    static constexpr int kMaxLength = 4096; // characters
    static constexpr int kMaxDepth = 64;    // nesting of brackets and operators

    // Returns false and fills `error` when the text is not a valid expression.
    static bool compile(std::string_view text, Formula& out, FormulaError& error);

    // Never fails: a result that is not a finite number is returned as it is (the callers below
    // turn it into zero).
    double evaluate(const FormulaVars& vars) const;

    int nodeCount() const { return nodes_; }
    bool empty() const { return code_.empty(); }

private:
    struct Instruction {
        std::uint8_t op = 0;
        std::uint8_t arg = 0; // variable or function index
        double value = 0.0;   // constants
    };
    std::vector<Instruction> code_;
    int nodes_ = 0;
    int stackDepth_ = 0;
    friend class FormulaParser;
};

enum class FormulaDomain { Wave, Harmonics };

struct FormulaRequest {
    FormulaDomain domain = FormulaDomain::Wave;
    std::string expression;      // wave: the sample value; harmonics: the level of harmonic h
    std::string phaseExpression; // harmonics only, optional: the phase of harmonic h in radians
    FrameSelection scope = allFrames(); // the frames written
    int selectedFrame = 0;       // what `sel` reads
    std::uint64_t seed = 1;      // for `rand` and noise()
    bool normalize = false;      // scale each written frame to a peak of 1
};

// Writes the frames in scope. Returns false with a message that names the position and the
// problem when an expression does not compile; the table is then unchanged.
bool applyFormula(Wavetable& table, const FormulaRequest& request, std::string& error);

} // namespace zed
