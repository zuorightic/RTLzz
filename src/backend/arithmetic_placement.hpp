#pragma once
#include "backend/arithmetic_general_state.hpp"
#include <optional>

namespace pred::beir::arithmetic {
struct TemplateIntervalCostQuery {
    int stage=0,first=0,last=0;
    std::string region,template_id,query_sha256;
    std::map<std::string,int> parameters;
    std::vector<NodeId> members,inputs,outputs;
    std::map<NodeId,std::vector<unsigned>> input_bit_positions,output_bit_positions;
};
struct CompletePlacementCostQuery {
    GeneralStateCostQuery state;
    std::vector<StageCostQuery> stages;
    std::vector<TemplateIntervalCostQuery> intervals;
    struct StorageBoundary {
        std::vector<std::size_t> launch_slots,capture_slots,live_through_capture_slots;
        std::string sha256;
    };
    std::vector<StorageBoundary> storage_boundaries;
    std::string query_sha256;
};
// Unknown is not a zero-valued price. A partial lower bound may prune a
// branch only when the loaded provider explicitly certifies its support.
struct PartialPlacementCostQuery {
    std::vector<int> placement; // -1 means unassigned.
    pipeline::Options options;
    ServiceStateProtocol service;
    double period_ps=0;
    std::string graph_sha256,query_sha256;
};
struct PartialPlacementBound {
    bool certified=false;
    std::optional<double> maximum_stage_delay_lower_ps,total_area_lower_um2;
    std::string query_sha256,model_sha256,reason;
};
struct StagePlacementPrice {
    int index=0;
    double QD_ps=0,clock_capture_overhead_ps=0,combinational_area_um2=0;
    std::string cut_sha256,state_boundary_sha256,model_sha256,ownership; // whole_stage or interval_stage
    std::string qualified_domain;
    std::vector<NodeId> owned_members;
    // A whole-stage G price can include interval logic once. I qualification
    // receipts are then required, with no additional interval area charge.
    std::map<std::string,std::string> interval_qualification_sha256;
};
struct CompletePlacementPrice {
    std::string query_sha256,model_sha256;
    std::vector<StagePlacementPrice> stages;
    std::map<std::string,double> functional_state_area_um2;
    std::map<std::string,std::string> state_owner_model_sha256;
    double shared_physical_area_um2=0;
    std::string shared_physical_model_sha256;
    // These paths include their own H+delay. External host IO is subtracted
    // once; registered service->core stages do not lose another0.2T.
    std::map<std::string,double> control_boundary_upper_ps;
    bool joint_source_and_boundary_qualified=false;
};
struct CompletePlacementEvaluation {
    CompletePlacementPrice price;
    std::vector<double> stage_budgets_ps,stage_delays_ps;
    std::map<std::string,double> boundary_margins_ps;
    double combinational_area_um2=0,functional_state_area_um2=0,total_area_um2=0,maximum_stage_delay_ps=0;
    std::uint64_t native_declared_bits=0,native_register_count=0,all_variable_state_bits=0,all_state_signals=0;
    bool feasible=false;
};
CompletePlacementCostQuery completePlacementCostQuery(const Program&,const pipeline::Options&,
    const std::vector<int>&,double,const ServiceStateProtocol&);
CompletePlacementCostQuery completePlacementCostQuery(const pipeline::Program&,double,const ServiceStateProtocol&);
PartialPlacementCostQuery partialPlacementCostQuery(const Program&,const pipeline::Options&,
    const std::vector<int>&,double,const ServiceStateProtocol&);
CompletePlacementEvaluation evaluateCompletePlacement(const Program&,const CompletePlacementCostQuery&,const CostProvider&);
CompletePlacementEvaluation evaluateCompletePlacement(const pipeline::Program&,double,const ServiceStateProtocol&,const CostProvider&);
PartialPlacementBound evaluatePartialPlacement(const Program&,const PartialPlacementCostQuery&,const CostProvider&);
std::string completePlacementCostQueryJson(const CompletePlacementCostQuery&);
std::string completePlacementEvaluationJson(const CompletePlacementEvaluation&);
}
