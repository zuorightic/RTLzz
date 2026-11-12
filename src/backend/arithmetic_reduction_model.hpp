#pragma once
#include "backend/arithmetic_reduction.hpp"
#include <memory>
namespace pred::beir::arithmetic {
struct ReductionPrice { double delay_ps=0,area_um2=0; };
enum class ReductionUse { Production, CandidateParity };
class ReductionCostModel {
    struct Impl;std::shared_ptr<Impl> impl_;
public:
    ReductionCostModel(const std::string& path,const std::string& sha256,const std::string& profile_sha256,
        ReductionUse use = ReductionUse::Production);
    ReductionPrice price(const ReductionRegion&,double target_ps) const;
};
}
