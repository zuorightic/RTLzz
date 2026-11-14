#pragma once
#include "backend/beir.hpp"
#include <llvm/Support/JSON.h>

namespace pred::beir::arithmetic {
inline constexpr const char* kLimbSplitPolicy = "unsigned-carry-chain-split-v1";
struct LimbSplitResult {
    Program graph;
    llvm::json::Array expansions;
};
// Rewrite unowned unsigned Add/Sub/AddCarry into exact little-endian limbs.
// A zero limb width preserves the graph. Protected region members are retained;
// their existing legal cuts remain authoritative. No timing prices are used.
LimbSplitResult splitUnsignedLimbs(Program, int limb_width);
}
