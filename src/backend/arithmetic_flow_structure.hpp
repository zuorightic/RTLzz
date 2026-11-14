#pragma once
#include "backend/arithmetic_centered_response.hpp"

namespace pred::beir::arithmetic {
NumericalFeatures flowStructureOperatorFeatures(const llvm::json::Object&);
llvm::json::Object flowStructureReductionFeatures(const llvm::json::Object&);
llvm::json::Array flowStructureStateFeatures(const llvm::json::Object&);
// Explicit diagnostics only; the inherited bundle supplies unchanged scope
// gates, not physical candidate prices or a public qualification.
llvm::json::Object evaluateFlowStructure(const llvm::json::Object&,
    const std::string& kind, const llvm::json::Object&, bool diagnostic = false);
}
