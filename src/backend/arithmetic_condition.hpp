#pragma once
#include "backend/arithmetic_operator_query.hpp"

namespace pred::beir::arithmetic {
// A local function estimate does not qualify S, G or a complete stage price.
struct OperatorConditionEstimate {
    bool covered=false;
    double operator_delay_ps=0, delay_reserve_ps=0, comb_area_um2=0;
    std::string reason;
};
OperatorConditionEstimate operatorConditionEstimate(const llvm::json::Object& model,
    const llvm::json::Object& descriptor,bool allow_candidate=false);
}
