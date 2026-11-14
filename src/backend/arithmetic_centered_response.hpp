#pragma once
#include <llvm/Support/JSON.h>
#include <map>
#include <string>
namespace pred::beir::arithmetic {
using NumericalFeatures = std::map<std::string, double>;
// Centers and scales are retained from training. Export never constructs a
// cancellation-prone raw intercept. This is a pure numerical operation.
double centeredResponse(const llvm::json::Object&, const NumericalFeatures&);
NumericalFeatures commonMechanismOperatorFeatures(const llvm::json::Object&);
llvm::json::Object commonMechanismReductionFeatures(const llvm::json::Object&);
llvm::json::Array commonMechanismStateFeatures(const llvm::json::Object&);
llvm::json::Object evaluateCommonCentered(const llvm::json::Object& model,
    const std::string& kind, const llvm::json::Object& query, bool diagnostic = false);
}
