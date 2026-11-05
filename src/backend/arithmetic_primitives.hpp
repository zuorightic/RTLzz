#pragma once
#include "backend/arithmetic_core.hpp"
#include <memory>

namespace pred::beir::arithmetic {
inline constexpr const char* kPublishedPrimitiveBundle="asap7-m2-primitives-static-composite-qualified-v1";
// Production component consumer. Missing stage/state/boundary qualification is
// an explicit D coverage failure; it is never replaced by a default price.
class PublishedPrimitiveCostProvider final : public CostProvider {
    struct Impl;
    std::shared_ptr<Impl> impl_;
protected:
    CostEstimate estimateGroup(const Program&,const std::vector<NodeId>&) const override;
public:
    PublishedPrimitiveCostProvider(const Program&,const rtlzz::ArithmeticRequest&);
    CostEstimate node(const Program&,NodeId) const override;
    CostEstimate nodeInStage(const Program&,NodeId,const StageCostQuery&) const override;
    bool requiresStageContext() const override { return true; }
    std::string componentReport(const Program&,const std::vector<int>& = {},int = 0,
                                const std::vector<NodeId>& = {}) const override;
    std::string modelIdentity() const override;
    double stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t) const override;
    void validateStateProtocol(bool,bool,bool) const override;
    void validateInterface(const Program&,const std::string&) const override;
    double dataBudget(bool) const override;
    double stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const override;
    double stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const override;
    bool stageCovered(std::size_t,std::uint64_t) const override { return false; }
    bool stageShapeCovered(const std::vector<NodeId>&) const override { return false; }
    bool monotoneGroupBounds() const override { return false; }
};
}
