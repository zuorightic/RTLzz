#pragma once
#include "backend/arithmetic_placement.hpp"
#include <memory>

namespace pred::beir::arithmetic {
inline constexpr const char* kJointComponentManifest="arithmetic-joint-component-manifest-v1";
struct ExactIntervalPrice {
    std::string domain_id,query_sha256,model_sha256,topology_sha256;
    double QD_ps=0,arithmetic_area_um2=0;
    std::map<std::string,double> features;
};
// Identity-free structural DAG witness. It covers operation types, widths,
// constants, input sharing and every reachable paid node, including FP internals.
std::string jointCoreFingerprint(const Program&,const std::string& input_valid,const std::string& output_valid);
class JointCompositeCostProvider final : public CostProvider {
    struct Impl;
    std::shared_ptr<Impl> impl_;
protected:
    CostEstimate estimateGroup(const Program&,const std::vector<NodeId>&) const override;
public:
    JointCompositeCostProvider(const Program&,const rtlzz::ArithmeticRequest&);
    CostEstimate node(const Program&,NodeId) const override;
    bool requiresStageContext() const override {return true;}
    bool requiresStateQuery() const override {return true;}
    bool requiresCompletePlacementCost() const override {return true;}
    CompletePlacementPrice completePlacementCost(const Program&,const CompletePlacementCostQuery&) const override;
    std::vector<ExactIntervalPrice> intervalPrices(const Program&,const CompletePlacementCostQuery&) const;
    void validateStateProtocol(bool,bool,bool) const override;
    void validateInterface(const Program&,const std::string&) const override;
    std::string componentReport(const Program&,const std::vector<int>& = {},int = 0,const std::vector<NodeId>& = {}) const override;
    std::string modelIdentity() const override;
    bool monotoneGroupBounds() const override {return false;}
    bool stageCovered(std::size_t,std::uint64_t) const override {return false;}
    bool stageShapeCovered(const std::vector<NodeId>&) const override {return false;}
};
std::string exactIntervalPricesJson(const std::vector<ExactIntervalPrice>&);
}
