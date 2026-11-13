#pragma once
#include "backend/arithmetic_wide_reduction.hpp"
namespace pred::beir::arithmetic {
struct MappingReductionFeatures : WideReductionFeatures { std::map<std::string,double> intensive; };
MappingReductionFeatures mappingReductionFeatures(const llvm::json::Object&, double target);
llvm::json::Object evaluateMappingReduction(const llvm::json::Object& model,
    const llvm::json::Object& query, bool diagnostic = false);
WideReductionFeatures wordReductionFeatures(const llvm::json::Object&, double target);
llvm::json::Object evaluateWordReduction(const llvm::json::Object& model,
    const llvm::json::Object& query, bool diagnostic = false);
}
