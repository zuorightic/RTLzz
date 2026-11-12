#pragma once
#include "backend/arithmetic_state_boundary.hpp"
namespace pred::beir::arithmetic {
// Same actual state/placement checks and frozen S coefficients as the original
// provider. Its outputs own S only; no internal node or shared price is queried.
class FactoredStateBoundaryProvider final : public ConditionalBoundaryProvider {
    struct Impl;std::shared_ptr<Impl> impl_;
public:
    FactoredStateBoundaryProvider(const std::string& binding,const std::string& sha256,
        const rtlzz::ArithmeticRequest&,StateBoundaryUse=StateBoundaryUse::RequireQualified);
    ConditionalBoundaryPrice price(const Program&,const StateCostQuery&,
        const std::vector<ConditionalBoundaryRequest>&,const std::string&) const override;
};
}
