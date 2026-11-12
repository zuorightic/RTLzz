#pragma once
#include "backend/arithmetic_operator_query.hpp"

namespace pred::beir::arithmetic {
inline constexpr const char* kReductionPolicy = "register-launched-reduction-v1";
struct ReductionBit { NodeId source; int bit, column; };
struct ReductionRegion {
    NodeId root = kInvalidNodeId;
    int stage = 0, width = 0;
    std::vector<NodeId> additions, members;
    std::vector<std::vector<ReductionBit>> terms;
    std::map<std::string,double> features;
};
// Pure discovery: no prices, source names, candidate IDs, or physical labels.
// Only independent, zero-padded, register-launched unsigned sum trees qualify.
// Unsafe intermediate modulo-then-widen, shared interiors, and same-stage
// non-Add producers keep their original operator boundaries.
std::vector<ReductionRegion> reductionRegions(const Program&,const std::vector<int>&,
    int last,const std::vector<NodeId>& controls = {});
std::map<std::string,double> reductionFeatures(int width,
    const std::vector<std::vector<ReductionBit>>&,double target_ps);
llvm::json::Object reductionRegionJson(const ReductionRegion&,double target_ps);
}
