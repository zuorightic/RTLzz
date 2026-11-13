#pragma once
#include "backend/arithmetic_reduction.hpp"
namespace pred::beir::arithmetic {
inline constexpr const char* kReductionMergeFeatureVersion="bit-column-features-v3-final-merge";
// Feature extraction only; no extension of a frozen G model's support/quality.
std::map<std::string,double> reductionMergeFeatures(int width,
    const std::vector<std::vector<ReductionBit>>&,double target_ps);
}
