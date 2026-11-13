#pragma once
#include "backend/arithmetic_wide_query.hpp"
namespace pred::beir::arithmetic {
std::map<std::string,double> wideFunctionQueryFeatures(const llvm::json::Object&);
// Diagnostic mode is explicit; the normal consumer requires independent
// qualification and the same measurement/profile/feature identity.
llvm::json::Object evaluateWideFunctionModel(const llvm::json::Object& model,
    const llvm::json::Object& query,bool diagnostic=false);
}
