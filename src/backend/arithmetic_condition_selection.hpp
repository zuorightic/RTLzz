#pragma once
#include "backend/arithmetic_condition_provider.hpp"
#include "backend/arithmetic_selection.hpp"
#include "backend/arithmetic_state.hpp"
#include <optional>
#include <set>
#include <limits>

namespace pred::beir::arithmetic {
using ConditionalEdge=std::pair<NodeId,NodeId>;
struct ConditionalBoundaryRequest {
    StageCostQuery cut;
    std::set<NodeId> launches,captures;
    std::set<ConditionalEdge> edges;
};
struct ConditionalStageBoundary {
    std::string cut_sha256;
    std::map<NodeId,double> launch_arrival_ps,capture_required_ps;
    std::map<ConditionalEdge,double> wire_delay_ps;
    // Covers service/control/bypass timing outside the arithmetic DAG. The
    // provider must explicitly include it, even when its qualified value is 0.
    double other_timing_violation_ps=std::numeric_limits<double>::quiet_NaN();
};
struct ConditionalBoundaryPrice {
    bool covered=false,joint_qualified=false;
    std::string reason,model_sha256,operator_model_sha256,layout_sha256;
    double target_period_ps=0;
    std::vector<ConditionalStageBoundary> stages;
    // Exactly these disjoint non-node owners, each charged once per placement:
    // input_service, native_state, external_state, shared_clock,
    // shared_physical, root_repair. Operators belong to the node function.
    std::map<std::string,double> owner_area_um2;
};
class ConditionalBoundaryProvider {
public:
    virtual ~ConditionalBoundaryProvider()=default;
    virtual ConditionalBoundaryPrice price(const Program&,const StateCostQuery&,
        const std::vector<ConditionalBoundaryRequest>&,const std::string& operator_model_sha256) const=0;
};
struct ConditionalView {
    std::string id,candidate_id;
    Program graph;
    std::vector<int> placement;
};
struct ConditionalViews {
    std::vector<ConditionalView> views;
    bool complete=false;
    std::size_t assignments=0,structures=0;
    std::string reason,candidate_policy,inventory_sha256;
};
// Fixed prices establish structural legality only. All resulting views are
// priced by the same conditional function and actual boundary provider.
ConditionalViews enumerateConditionalViews(const Program&,const pipeline::Options& structural_options,
    const rtlzz::ArithmeticRequest& candidate_policy,std::size_t assignment_limit=1000000);
struct ConditionalSelection {
    std::optional<pipeline::Program> pipeline;
    std::string view_id,report;
};
// No EDA, fallback prices, or target-dependent removal of structural views.
// Production refuses incomplete enumeration/coverage or unqualified boundaries.
ConditionalSelection selectConditionalViews(const ConditionalViews&,const pipeline::Options& structural_options,
    const rtlzz::ArithmeticRequest& model_request,const ConditionalBoundaryProvider&,
    ConditionalUse=ConditionalUse::Production);
}
