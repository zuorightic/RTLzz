#pragma once
#include "backend/arithmetic_workload_plan.hpp"

namespace pred::beir::arithmetic {
// Shared operator/G production consumer for the new graph ABI. Every unsupported
// observation or unqualified model is cost_uncovered. The explicit diagnostic
// switch never grants permission to select public D.
class WorkloadCostProvider final : public CostProvider {
    WorkloadPlan plan_;
    std::vector<int> placement_;
    int last_stage_ = 0;
    std::map<NodeId,CostEstimate> prices_;
    llvm::json::Object report_;
    bool sameGraph(const Program&)const;
public:
    WorkloadCostProvider(const pipeline::Program&,const WorkloadPlan&,
                         const llvm::json::Object& models,bool diagnostic=false);
    CostEstimate node(const Program&,NodeId)const override;
    CostEstimate nodeInStage(const Program&,NodeId,const StageCostQuery&)const override;
    bool requiresStageContext()const override{return true;}
    bool requiresStateQuery()const override{return true;}
    StateCostEstimate stateCost(const Program&,const StateCostQuery&)const override;
    double stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t)const override;
    double dataBudget(bool)const override;
    bool stageCovered(std::size_t,std::uint64_t)const override{return false;}
    bool stageShapeCovered(const std::vector<NodeId>&)const override{return false;}
    bool monotoneGroupBounds()const override{return false;}
    double stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>&)const override;
    double stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>&)const override;
    std::string modelIdentity()const override{return "workload-common-provider-v2";}
    std::string componentReport(const Program&,const std::vector<int>& = {},int = 0,
                               const std::vector<NodeId>& = {})const override;
};
}
