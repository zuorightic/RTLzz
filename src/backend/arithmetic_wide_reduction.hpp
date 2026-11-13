#pragma once
#include "backend/arithmetic_prefix_features.hpp"
namespace pred::beir::arithmetic {
struct WideReductionFeatures {
    std::map<std::string,double> area,delay;
};
WideReductionFeatures wideReductionFeatures(const llvm::json::Object&,double target);
llvm::json::Object evaluateWideReduction(const llvm::json::Object& model,
    const llvm::json::Object& query,bool diagnostic=false,bool extrapolate=false);
}
