#pragma once
#include "backend/arithmetic_core.hpp"
#include <memory>
namespace pred::beir::arithmetic {
// Portable fitted coefficients/trees only. Construction/query never runs a
// Python process or physical backend and never substitutes the fixed table.
class ModelCostProvider final : public CostProvider {
    struct Impl;
    std::shared_ptr<Impl> impl_;
    std::optional<double> templateIntervalCost(const std::vector<NodeId>&,bool) const;
protected:
    CostEstimate estimateGroup(const Program&,const std::vector<NodeId>&) const override;
public:
    ModelCostProvider(const Program&, const rtlzz::ArithmeticRequest&);
    CostEstimate node(const Program&,NodeId) const override;
    double stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t) const override;
    void validateStateProtocol(bool,bool,bool) const override;
    void validateInterface(const Program&,const std::string&) const override;
    double dataBudget(bool) const override;
    double stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const override;
    double stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>& = {}) const override;
    bool monotoneGroupBounds() const override;
    bool stageCovered(std::size_t,std::uint64_t) const override;
    bool stageShapeCovered(const std::vector<NodeId>&) const override;
    std::string modelIdentity() const override;
};
}
