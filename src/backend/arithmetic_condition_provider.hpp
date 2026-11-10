#pragma once
#include "backend/arithmetic_condition.hpp"

namespace pred::beir::arithmetic {
inline constexpr const char* kConditionalOperatorModel="operator-condition-function-v1";
enum class ConditionalUse { Production, TestOnly };
struct ConditionalNodePrice {
    OperatorQuery query;
    OperatorConditionEstimate price;
};

// Loads the exact step-5 function file. It grants node prices only; an actual
// stage/state boundary provider must independently qualify a full selection.
class ConditionalOperatorCostProvider final : public CostProvider {
    struct Impl;
    std::shared_ptr<Impl> impl_;
protected:
    CostEstimate estimateGroup(const Program&,const std::vector<NodeId>&) const override;
public:
    ConditionalOperatorCostProvider(const Program&,const rtlzz::ArithmeticRequest&,
                                    ConditionalUse=ConditionalUse::Production);
    std::vector<ConditionalNodePrice> placementPrices(const Program&,const std::vector<int>&,
                                                     int,const std::vector<NodeId>&) const;
    CostEstimate node(const Program&,NodeId) const override;
    CostEstimate nodeInStage(const Program&,NodeId,const StageCostQuery&) const override;
    bool requiresStageContext() const override { return true; }
    bool requiresStateQuery() const override { return true; }
    bool monotoneGroupBounds() const override { return false; }
    bool stageCovered(std::size_t,std::uint64_t) const override { return false; }
    bool stageShapeCovered(const std::vector<NodeId>&) const override { return false; }
    std::string modelIdentity() const override;
    std::string componentReport(const Program&,const std::vector<int>& = {},int = 0,
                                const std::vector<NodeId>& = {}) const override;
    void validateInterface(const Program&,const std::string&) const override;
    double stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t) const override;
    double dataBudget(bool) const override;
    double stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const override;
    double stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const override;
};
}
