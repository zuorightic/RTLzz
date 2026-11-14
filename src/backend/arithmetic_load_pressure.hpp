#pragma once
#include "backend/arithmetic_rebuffer_context.hpp"

namespace pred::beir::arithmetic {
NumericalFeatures loadPressureOperatorFeatures(const llvm::json::Object&);
llvm::json::Object loadPressureReductionFeatures(const llvm::json::Object&);
llvm::json::Array loadPressureStateFeatures(const llvm::json::Object&, const llvm::json::Object&);
// Explicit diagnostics only; the inherited bundle supplies unchanged scope
// gates, not physical candidate prices or a public qualification.
llvm::json::Object evaluateLoadPressure(const llvm::json::Object&,
    const std::string& kind, const llvm::json::Object&, bool diagnostic = false);
}
