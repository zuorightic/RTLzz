#pragma once
#include "backend/arithmetic_wide_function.hpp"
namespace pred::beir::arithmetic {
std::map<std::string,double> successorOperatorFeatures(const llvm::json::Object&);
llvm::json::Object evaluateOperatorSuccessor(const llvm::json::Object& model,
    const llvm::json::Object& query,bool diagnostic=false);
}
