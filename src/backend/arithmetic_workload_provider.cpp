#include "backend/arithmetic_workload_provider.hpp"
#include "backend/arithmetic_operator_successor.hpp"
#include "backend/arithmetic_word_reduction.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <cmath>
#include <stdexcept>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;
CostEstimate missing(const std::string& reason){CostEstimate c;c.covered=false;c.source="workload-common-provider-v2";c.reason=reason;c.delay_unit="ps";c.area_unit="um2";return c;}
O rejected(const std::string& why){return O{{"status","cost_uncovered"},{"reason",why}};}
CostEstimate estimate(const O& answer){auto status=answer.getString("status");
    if(status!="covered"&&status!="diagnostic_estimate")return missing(answer.getString("reason").value_or("missing component coverage").str());
    auto d=answer.getNumber("operator_delay_ps"),a=answer.getNumber("comb_area_um2");
    if(!d||!a||!std::isfinite(*d)||!std::isfinite(*a)||*d<=0||*a<=0)return missing("invalid component price");
    CostEstimate c;c.delay=*d;c.combinational=*a;c.source="workload-common-provider-v2";c.delay_unit="ps";c.area_unit="um2";return c;
}
[[noreturn]] void jointMissing(){throw std::runtime_error("workload S/shared and full-stage qualification missing; no inherited static boundary price");}
}
WorkloadCostProvider::WorkloadCostProvider(const pipeline::Program& p,const WorkloadPlan& plan,const O& models,bool diagnostic)
    :plan_(plan),placement_(p.stages),last_stage_(p.options.register_stages){
    auto actual=workloadPlan(p,plan.target_ps,plan.profile_sha256);
    if(llvm::json::Value(workloadPlanJson(actual))!=llvm::json::Value(workloadPlanJson(plan)))throw std::runtime_error("provider received stale workload plan");
    if(!models.empty()&&(models.getString("version")!="workload-model-bindings-v2"||models.getString("profile_sha256")!=plan.profile_sha256))
        throw std::runtime_error("workload model binding version/profile mismatch");
    context_.cost_mode="backend-model";context_.target_platform="asap7";context_.platform_profile_sha256=plan.profile_sha256;
    A rows;std::size_t covered=0,missing_count=0;double partial=0;
    auto add=[&](NodeId n,const char* kind,O answer){auto c=estimate(answer);covered+=c.covered;missing_count+=!c.covered;if(c.covered)partial+=c.combinational;
        prices_.emplace(n,c);rows.push_back(O{{"node",n},{"owner",kind},{"result",std::move(answer)}});};
    for(const auto& row:plan.operators){O answer;
        if(!row.predecessor_query)answer=rejected("new context requires qualified successor: "+row.predecessor_rejection);
        else if(auto model=models.getObject("operators"))answer=evaluateOperatorSuccessor(*model,*row.predecessor_query,diagnostic);
        else answer=rejected("shared operator model not supplied");
        add(row.node,"operator",std::move(answer));
    }
    for(const auto& region:plan.reductions){O answer;
        const auto& query=plan.reduction_queries.at(region.root);
        if(query.getInteger("shared_input_root_pairs").value_or(0)>0)answer=rejected("correlated G sources require boundary qualification");
        else if(auto model=models.getObject("G"))answer=evaluateMappingReduction(*model,query,diagnostic);
        else answer=rejected("shared G model not supplied");
        add(region.root,"G",std::move(answer));
        for(auto n:region.members)if(n!=region.root){CostEstimate c;c.deferred_to_stage=true;c.source="owned-by-G-region";c.delay_unit="ps";c.area_unit="um2";prices_.emplace(n,std::move(c));}
    }
    for(auto n:plan.functional_logic)add(n,"functional",rejected("functional non-wire logic needs an explicitly qualified provider"));
    report_=O{{"version","workload-common-provider-v2"},{"mode",diagnostic?"diagnostic":"public"},{"owners",std::move(rows)},
        {"component_covered",covered},{"component_uncovered",missing_count},{"covered_comb_area_partial_um2",partial},
        {"stage_status","cost_uncovered"},{"stage_reason","S/shared and actual arithmetic/full-stage qualification pending"},
        {"width_routing",false},{"static_fallback",false},{"candidate_PPA_calls",0},{"model_selection_performed",false},{"full_D_ready",false}};
}
bool WorkloadCostProvider::sameGraph(const Program& p)const{
    return rtlzz::arithmeticHash(emitText(p))==plan_.graph_sha256;
}
CostEstimate WorkloadCostProvider::node(const Program& p,NodeId n)const{
    if(!sameGraph(p))return missing("graph changed: rebuild workload plan and provider");
    auto found=prices_.find(n);if(found!=prices_.end())return found->second;
    const auto& s=p.signal(n);const auto& d=*s.driver;
    if((s.value.valid&&s.value.constant)||d.kind==OperationKind::PortRead||d.kind==OperationKind::Assign||d.kind==OperationKind::Cast||d.kind==OperationKind::ZExt||d.kind==OperationKind::Trunc||d.kind==OperationKind::Slice||d.kind==OperationKind::BitSelect||d.kind==OperationKind::Concat){CostEstimate c;c.source="exact-constant-or-wire";c.delay_unit="ps";c.area_unit="um2";return c;}
    return missing("node is outside priced workload owners; explicit dead-bit/alias lowering required");
}
CostEstimate WorkloadCostProvider::nodeInStage(const Program& p,NodeId n,const StageCostQuery& q)const{
    if(q.placement!=placement_||q.last!=last_stage_||n<0||std::size_t(n)>=placement_.size()||q.index!=placement_[n])
        return missing("placement/stage changed: rebuild workload plan and provider");
    auto actual=stageCostQuery(p,q.placement,q.index,q.last,q.controls);
    bool captures=actual.captures.size()==q.captures.size();
    for(std::size_t i=0;captures&&i<actual.captures.size();++i)
        captures=actual.captures[i].value==q.captures[i].value&&actual.captures[i].banks==q.captures[i].banks;
    if(actual.graph_sha256!=plan_.graph_sha256||q.graph_sha256!=plan_.graph_sha256||actual.cut_sha256!=q.cut_sha256||actual.members!=q.members||!captures)
        return missing("stale/forged workload stage context");
    return node(p,n);
}
StateCostEstimate WorkloadCostProvider::stateCost(const Program&,const StateCostQuery&)const{jointMissing();}
double WorkloadCostProvider::stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t)const{jointMissing();}
double WorkloadCostProvider::dataBudget(bool)const{jointMissing();}
double WorkloadCostProvider::stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>&)const{jointMissing();}
double WorkloadCostProvider::stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>&)const{jointMissing();}
std::string WorkloadCostProvider::componentReport(const Program& p,const std::vector<int>& where,int,const std::vector<NodeId>&)const{
    if(!sameGraph(p))throw std::runtime_error("stale workload component report graph");
    if(!where.empty()&&where!=placement_)throw std::runtime_error("stale workload component report placement");
    return llvm::formatv("{0:2}",llvm::json::Value(O(report_))).str()+"\n";
}
}
