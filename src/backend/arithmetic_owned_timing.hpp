#pragma once
#include "backend/arithmetic_state_boundary.hpp"

namespace pred::beir::arithmetic {
// A prospective composition policy.  It consumes qualified node point prices,
// preserves real S state/control accounting, and never grants joint quality.
class OwnedTimingCostProvider {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    OwnedTimingCostProvider(const std::string& S_binding,const std::string& S_sha256,
        const rtlzz::ArithmeticRequest&,const std::string& timing_policy,const std::string& timing_sha256);
    llvm::json::Object estimate(const Program&,const StateCostQuery&,
        const std::vector<ConditionalBoundaryRequest>&) const;
};
// Full prediction-time bit operations for an independent language oracle.
llvm::json::Object ownedTimingGraphJson(const Program&);
}
