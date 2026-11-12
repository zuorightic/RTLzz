#include "backend/arithmetic_fused_selection.hpp"
#include "backend/arithmetic_fused_cost.hpp"
#include "backend/arithmetic_shared_function_v2.hpp"
#include "backend/arithmetic_operator_query.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <sstream>
#include <tuple>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
void require(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
double number(const O& o,const char* key){auto v=o.getNumber(key);require(v&&std::isfinite(*v)&&*v>=0,std::string("invalid D estimate ")+key);return *v;}
std::string inventoryHash(const ConditionalViews& inventory){
    std::ostringstream s;s<<inventory.candidate_policy<<'/'<<inventory.complete<<'/'<<inventory.assignments<<'/'<<inventory.structures;
    for(const auto& view:inventory.views){s<<'/'<<view.id<<'/'<<view.candidate_id<<'/'<<rtlzz::arithmeticHash(emitText(view.graph));for(auto at:view.placement)s<<':'<<at;}
    return rtlzz::arithmeticHash(s.str());
}
}
ConditionalSelection selectFusedD(const ConditionalViews& inventory,const pipeline::Options& options,
    const rtlzz::ArithmeticRequest& request,const FusedDContext& context){
    auto start=std::chrono::steady_clock::now();
    require(context.output_scope=="prototype-no-reset-timing-v1","unsupported D output scope");
    rtlzz::validateArithmeticRequest(request,options.register_stages);
    require(request.cost_mode=="backend-model"&&request.latency==options.register_stages+1&&request.initiation_interval==1,"D L/II/model context differs");
    require(options.arithmetic_request&&options.arithmetic_request->cost_mode=="fixed","D needs a fixed structural enumeration context");
    const auto& fixed=*options.arithmetic_request;
    require(fixed.target_platform==request.target_platform&&fixed.platform_profile_sha256==request.platform_profile_sha256&&fixed.operator_boundary_policy==request.operator_boundary_policy,"D structural/physical flow mismatch");
    require(inventory.inventory_sha256==inventoryHash(inventory)&&inventory.candidate_policy==request.candidate_policy,"D structural inventory changed");
    FusedFunctionCostProvider costs(request,context);
    A attempts;std::set<std::string> ids;bool complete=inventory.complete&&!inventory.views.empty();
    struct Scored {std::size_t index;double area,violation;};std::vector<Scored> scored;
    auto structural=options;structural.arithmetic_request->target_period_ps=request.target_period_ps;
    for(std::size_t i=0;i<inventory.views.size();++i){const auto& view=inventory.views[i];require(ids.insert(view.id).second,"duplicate D candidate view");
        O row{{"view",view.id},{"candidate",view.candidate_id},{"status","cost_uncovered"}};A placement;for(int at:view.placement)placement.push_back(at);row["placement"]=std::move(placement);
        try{
            auto materialized=pipeline::materializeSchedule(view.graph,structural,view.placement);pipeline::validateProgram(materialized);
            std::vector<NodeId> controls;
            for(const auto& name:{options.input_valid_name,options.output_valid_name}){
                auto p=std::find_if(view.graph.ports.begin(),view.graph.ports.end(),[&](const auto& p){return p.name==name&&p.element_nodes.size()==1;});
                require(p!=view.graph.ports.end(),"D missing actual valid port");controls.push_back(p->element_nodes.front());
            }
            auto cuts=conditionalBoundaryRequests(view.graph,view.placement,options.register_stages,controls);
            A exported_cuts,queries,ports;
            for(const auto& cut:cuts){A members,launches,captures,ctrl,edges;for(auto n:cut.cut.members)members.push_back(n);for(auto n:cut.launches)launches.push_back(n);for(auto n:cut.captures)captures.push_back(O{{"node",n}});for(auto n:controls)ctrl.push_back(n);for(auto e:cut.edges)edges.push_back(A{e.first,e.second});
                exported_cuts.push_back(O{{"stage",cut.cut.index},{"sha256",cut.cut.cut_sha256},{"members",std::move(members)},{"launches",std::move(launches)},{"captures",std::move(captures)},{"controls",std::move(ctrl)},{"edges",std::move(edges)}});}
            for(const auto& q:operatorQueries(view.graph,view.placement,options.register_stages,request.target_period_ps,request.platform_profile_sha256,request.operator_boundary_policy,controls))queries.push_back(*llvm::json::parse(operatorQueryJson(q)));
            for(const auto& p:view.graph.ports){A nodes;for(auto n:p.element_nodes)nodes.push_back(n);ports.push_back(O{{"name",p.name},{"width",p.type.width},{"input",p.direction==PortDirection::Input},{"nodes",std::move(nodes)}});}
            auto state=stateCostQuery(materialized,request.target_period_ps,true);auto quote=costs.estimate(view.graph,state,cuts);
            require(quote.getString("status")=="candidate_covered",quote.getString("reason")?quote.getString("reason")->str():"D estimate uncovered");
            require(quote.getString("layout_sha256")==state.layout_sha256&&quote.getNumber("target_ps")==request.target_period_ps&&quote.getString("operator_model_sha256")==request.model_bundle_sha256,"stale D numerical/placement/node identity");
            const auto* stages=quote.getArray("stages");require(stages&&stages->size()==cuts.size(),"D stage inventory incomplete");
            for(std::size_t j=0;j<cuts.size();++j){auto s=(*stages)[j].getAsObject();require(s&&s->getInteger("stage")==cuts[j].cut.index&&s->getString("cut_sha256")==cuts[j].cut.cut_sha256,"D stage identity differs");}
            double area=number(quote,"total_area_um2"),violation=number(quote,"decision_violation_ps");
            require(quote.getString("G_model_sha256")==context.G_model_sha256&&quote.getString("reduction_policy")==kReductionPolicy,"D G region identity differs");
            row["status"]="covered";row["total_area_um2"]=area;row["timing_violation_ps"]=violation;
            row["estimate"]=std::move(quote);row["state_query"]=*llvm::json::parse(stateCostQueryJson(state));
            row["graph"]=sharedFunctionGraphJsonV2(view.graph);row["cuts"]=std::move(exported_cuts);row["queries"]=std::move(queries);row["ports"]=std::move(ports);scored.push_back({i,area,violation});
        }catch(const std::exception& e){complete=false;row["reason"]=std::string(e.what());}
        attempts.push_back(std::move(row));
    }
    bool feasible=std::any_of(scored.begin(),scored.end(),[](const auto& x){return x.violation==0;});
    std::optional<Scored> best;std::optional<std::tuple<double,double,std::string>> score;
    if(complete)for(const auto& x:scored){if(feasible&&x.violation>0)continue;
        auto key=std::make_tuple(feasible?0.:x.violation,x.area,inventory.views[x.index].id);if(!score||key<*score){score=key;best=x;}}
    ConditionalSelection selected;
    if(best){const auto& view=inventory.views[best->index];selected.view_id=view.id;selected.pipeline=pipeline::materializeSchedule(view.graph,structural,view.placement);pipeline::validateProgram(*selected.pipeline);}
    O report{{"version","fused-function-D-selection-v1"},{"output_scope",context.output_scope},{"target_period_ps",request.target_period_ps},{"L",request.latency},{"II",request.initiation_interval},
        {"reset_timing_checked",false},{"functional_reset_preserved",true},{"joint_independent_qualified",false},{"full_D_ready",false},{"formal_D_selection",false},{"prototype_D_selection",bool(best)},
        {"selection_status",!complete?"cost_uncovered":feasible?"predicted_feasible_selected":"target_unmet"},{"coverage_complete",complete},{"structural_enumeration_complete",inventory.complete},
        {"view_count",std::uint64_t(inventory.views.size())},{"structure_count",std::uint64_t(inventory.structures)},{"structural_assignments",std::uint64_t(inventory.assignments)},
        {"inventory_sha256",inventory.inventory_sha256},{"candidate_policy",request.candidate_policy},{"global_architecture_optimum_claimed",false},{"candidate_PPA_calls",0},
        {"operator_model_sha256",request.model_bundle_sha256},{"S_binding_sha256",context.S_binding_sha256},{"composition_policy_sha256",context.composition_policy_sha256},{"G_model_sha256",context.G_model_sha256},{"reduction_policy",kReductionPolicy},
        {"decision_rule","feasible: minimum total area; otherwise minimum worst violation then area; deterministic view ID tie"},{"candidates",std::move(attempts)},
        {"selection_ms",std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count()}};
    if(best){report["selected_view"]=selected.view_id;report["selected_candidate"]=inventory.views[best->index].candidate_id;report["selected_area_um2"]=best->area;report["selected_timing_violation_ps"]=best->violation;}
    if(!inventory.complete)report["enumeration_reason"]=inventory.reason;
    selected.report=llvm::formatv("{0:2}",V(std::move(report))).str();return selected;
}
}
