#pragma once
#include "backend/arithmetic_condition_selection.hpp"

namespace pred::beir::arithmetic {
enum class StateBoundaryUse { RequireQualified, CandidateReuse };
// Real state/placement projection and frozen S parameters. CandidateReuse
// explicitly permits a diagnostic reuse estimate while new flow/joint gaps
// remain unqualified; it cannot grant a production D selection.
class ReusedStateBoundaryProvider final : public ConditionalBoundaryProvider {
    struct Impl;
    std::shared_ptr<Impl> impl_;
public:
    ReusedStateBoundaryProvider(const std::string& binding_path,const std::string& binding_sha256,
        const rtlzz::ArithmeticRequest&,StateBoundaryUse=StateBoundaryUse::RequireQualified);
    ConditionalBoundaryPrice price(const Program&,const StateCostQuery&,
        const std::vector<ConditionalBoundaryRequest>&,const std::string& operator_model_sha256) const override;
};
}
