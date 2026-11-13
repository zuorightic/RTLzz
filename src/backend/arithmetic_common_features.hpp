#pragma once
#include "backend/arithmetic_operator_query.hpp"

namespace pred::beir::arithmetic {
// Numerical feature and coefficient helpers only; no price qualification.
std::map<std::string,double> commonOperatorFeatures(const llvm::json::Object&);
std::map<std::string,double> logicalInputLoadFeatures(const std::vector<int>& widths,
    const std::vector<std::vector<int>>& sink_inputs);
double commonNumericalPrediction(const llvm::json::Object& model,
    const std::map<std::string,double>& features);
}
