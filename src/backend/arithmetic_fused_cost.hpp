#pragma once
#include "backend/arithmetic_fused_selection.hpp"
#include "backend/arithmetic_shared_function_v2.hpp"
#include "backend/arithmetic_reduction_model.hpp"
#include "backend/arithmetic_factored_state.hpp"
namespace pred::beir::arithmetic {
class FusedFunctionCostProvider {
    FactoredStateBoundaryProvider state_;ReductionCostModel regions_;
    rtlzz::ArithmeticRequest request_;FusedDContext context_;
public:
    FusedFunctionCostProvider(const rtlzz::ArithmeticRequest&,const FusedDContext&);
    llvm::json::Object estimate(const Program&,const StateCostQuery&,
        const std::vector<ConditionalBoundaryRequest>&) const;
};
}
