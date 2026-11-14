#pragma once
#include "backend/arithmetic_flow_structure.hpp"

namespace pred::beir::arithmetic {
NumericalFeatures branchLoadOperatorFeatures(const llvm::json::Object&);
llvm::json::Object branchLoadReductionFeatures(const llvm::json::Object&);
llvm::json::Array branchLoadStateFeatures(const llvm::json::Object&);
// Explicit diagnostics only; the inherited bundle supplies unchanged scope
// gates, not physical candidate prices or a public qualification.
llvm::json::Object evaluateBranchLoad(const llvm::json::Object&,
    const std::string& kind, const llvm::json::Object&, bool diagnostic = false);
}
