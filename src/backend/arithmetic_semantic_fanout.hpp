#pragma once
#include "backend/arithmetic_load_pressure.hpp"

namespace pred::beir::arithmetic {
NumericalFeatures semanticFanoutOperatorFeatures(const llvm::json::Object&);
llvm::json::Object semanticFanoutReductionFeatures(const llvm::json::Object&);
llvm::json::Array semanticFanoutStateFeatures(const llvm::json::Object&, const llvm::json::Object&);
// Explicit diagnostics only; the inherited bundle supplies unchanged scope
// gates, not physical candidate prices or a public qualification.
llvm::json::Object evaluateSemanticFanout(const llvm::json::Object&,
    const std::string& kind, const llvm::json::Object&, bool diagnostic = false);
}
