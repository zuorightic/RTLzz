#include "backend/arithmetic_condition_selection.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <sstream>
#include <tuple>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
[[noreturn]] void fail(const std::string& reason) {throw rtlzz::ArithmeticError({"conditional_selection","cost_uncovered",reason,{},{}});}
void finite(double v,const char* what,bool nonnegative=true) {if(!std::isfinite(v)||(nonnegative&&v<0))fail(std::string("invalid conditional ")+what);}
bool hash(const std::string& s) {return s.size()==64&&s.find_first_not_of("0123456789abcdef")==std::string::npos;}
template<class K> std::set<K> keys(const std::map<K,double>& m) {std::set<K> r;for(const auto& item:m)r.insert(item.first);return r;}
std::string inventoryIdentity(const ConditionalViews& inventory) {
    std::ostringstream s;s<<inventory.candidate_policy<<'/'<<inventory.complete<<'/'<<inventory.assignments<<'/'<<inventory.structures;
    for(const auto& view:inventory.views){s<<'/'<<view.id<<'/'<<view.candidate_id<<'/'<<rtlzz::arithmeticHash(emitText(view.graph));for(auto at:view.placement)s<<':'<<at;}
    return rtlzz::arithmeticHash(s.str());
}
std::vector<NodeId> controls(const Program& p,const pipeline::Options& o) {
    std::vector<NodeId> ids;
    for(const auto& name:{o.input_valid_name,o.output_valid_name}) {
        auto found=kInvalidNodeId;
        for(const auto& port:p.ports)if(port.name==name&&port.element_nodes.size()==1)found=port.element_nodes.front();
        if(found==kInvalidNodeId)fail("explicit actual valid ports required");ids.push_back(found);
    }return ids;
}
std::vector<ConditionalBoundaryRequest> boundaryRequests(const Program& p,const std::vector<int>& placement,int last,const std::vector<NodeId>& control) {
    std::vector<ConditionalBoundaryRequest> requests;
    for(int at=0;at<=last;++at) {
        ConditionalBoundaryRequest r;r.cut=stageCostQuery(p,placement,at,last,control);
        std::set<NodeId> inside(r.cut.members.begin(),r.cut.members.end());
        for(auto id:r.cut.members)for(const auto& operand:p.signal(id).driver->operands)if(operand.kind==OperandKind::Symbol) {
            const auto& s=p.signal(operand.node);if(s.value.valid&&s.value.constant)continue;
            r.edges.insert({operand.node,id});if(!inside.count(operand.node))r.launches.insert(operand.node);
        }
        for(const auto& capture:r.cut.captures) {
            r.captures.insert(capture.value);if(!inside.count(capture.value))r.launches.insert(capture.value);
        }
        requests.push_back(std::move(r));
    }return requests;
}
struct Evaluation {
    bool covered=false,qualified=false;double area=0,violation=0;
    std::optional<pipeline::Program> materialized;
    O report;
};
Evaluation evaluate(const ConditionalView& view,const pipeline::Options& options,const rtlzz::ArithmeticRequest& request,
                    const ConditionalBoundaryProvider& boundaries,ConditionalUse use) {
    Evaluation result;result.report=O{{"view",view.id},{"candidate",view.candidate_id},{"status","cost_uncovered"}};
    A placement;for(auto at:view.placement)placement.push_back(at);result.report["placement"]=std::move(placement);
    try {
        ConditionalOperatorCostProvider provider(view.graph,request,use);
        auto materialized=pipeline::materializeSchedule(view.graph,options,view.placement);pipeline::validateProgram(materialized);
        const auto& p=materialized.combinational;
        const auto ctrl=controls(p,options);const auto rows=provider.placementPrices(p,view.placement,options.register_stages,ctrl);
        std::map<NodeId,double> delays;double node_area=0;A prices;
        for(const auto& row:rows) {
            if(!row.price.covered)fail("node "+std::to_string(row.query.origin)+": "+row.price.reason);
            const auto& value=row.price;finite(value.operator_delay_ps,"node delay");finite(value.delay_reserve_ps,"node reserve");finite(value.comb_area_um2,"node area");
            if(!delays.emplace(row.query.origin,value.operator_delay_ps+value.delay_reserve_ps).second)fail("duplicate operator charge");
            node_area+=value.comb_area_um2;
            prices.push_back(O{{"node",row.query.origin},{"stage",row.query.stage},{"operator_delay_ps",value.operator_delay_ps},
                {"delay_reserve_ps",value.delay_reserve_ps},{"comb_area_um2",value.comb_area_um2},{"descriptor",O(row.query.descriptor)}});
        }
        if(rows.empty())fail("no demanded arithmetic nodes in conditional view");
        auto queries=boundaryRequests(p,view.placement,options.register_stages,ctrl);
        // Every actual non-wire member must own one qualified operator price.
        for(const auto& q:queries)for(auto id:q.cut.members) {
            auto kind=provider.node(p,id);if(!kind.covered)fail(kind.reason);
            if(kind.deferred_to_stage&&!delays.count(id))fail("unpriced demanded arithmetic member");
        }
        auto state=stateCostQuery(materialized,request.target_period_ps,true);
        auto quote=boundaries.price(p,state,queries,request.model_bundle_sha256);
        if(!quote.covered)fail("S/shared/boundary coverage: "+quote.reason);
        if(quote.operator_model_sha256!=request.model_bundle_sha256||quote.layout_sha256!=state.layout_sha256||
           quote.target_period_ps!=request.target_period_ps||!hash(quote.model_sha256))fail("stale boundary dependency/state/target identity");
        if(use==ConditionalUse::Production&&!quote.joint_qualified)fail("joint S/G/boundary price is not independently qualified");
        const std::set<std::string> owners{"input_service","native_state","external_state","shared_clock","shared_physical","root_repair"};
        if(keys(quote.owner_area_um2)!=owners)fail("non-node area ownership missing, duplicated or overlapping operators");
        O owner_json{{"operators",node_area}};double total=node_area;
        for(const auto& [owner,area]:quote.owner_area_um2){finite(area,"owner area");total+=area;owner_json[owner]=area;}
        finite(total,"total area");if(quote.stages.size()!=queries.size())fail("incomplete stage boundary inventory");
        double violation=0;A stages;
        for(std::size_t at=0;at<queries.size();++at) {
            const auto& q=queries[at];const auto& b=quote.stages[at];
            if(b.cut_sha256!=q.cut.cut_sha256||keys(b.launch_arrival_ps)!=q.launches||keys(b.capture_required_ps)!=q.captures||keys(b.wire_delay_ps)!=q.edges)
                fail("stage launches/captures/wires are incomplete or stale");
            for(const auto& [id,value]:b.launch_arrival_ps)finite(value,"launch arrival");
            for(const auto& [id,value]:b.capture_required_ps)finite(value,"capture required",false);
            for(const auto& [edge,value]:b.wire_delay_ps)finite(value,"inter-node wire");
            finite(b.other_timing_violation_ps,"non-arithmetic timing violation");
            double local=b.other_timing_violation_ps;
            std::set<NodeId> inside(q.cut.members.begin(),q.cut.members.end()),visiting;std::map<NodeId,double> arrival;
            std::function<double(NodeId)> propagate=[&](NodeId id) {
                const auto& s=p.signal(id);if(s.value.valid&&s.value.constant)return 0.;
                if(auto launch=b.launch_arrival_ps.find(id);launch!=b.launch_arrival_ps.end())return launch->second;
                if(arrival.count(id))return arrival.at(id);
                if(!inside.count(id)||!visiting.insert(id).second)fail("unpriced launch or cyclic conditional DAG");
                double maximum=0;
                for(const auto& operand:s.driver->operands)if(operand.kind==OperandKind::Symbol&&!(p.signal(operand.node).value.valid&&p.signal(operand.node).value.constant))
                    maximum=std::max(maximum,propagate(operand.node)+b.wire_delay_ps.at({operand.node,id}));
                auto node=delays.find(id);double value=maximum+(node==delays.end()?0.:node->second);finite(value,"DAG arrival");
                visiting.erase(id);return arrival[id]=value;
            };
            A endpoints;
            for(auto id:q.captures){auto value=propagate(id);double slack=b.capture_required_ps.at(id)-value;local=std::max(local,-slack);
                endpoints.push_back(O{{"node",id},{"arrival_ps",value},{"required_ps",b.capture_required_ps.at(id)},{"slack_ps",slack}});}
            violation=std::max(violation,local);
            stages.push_back(O{{"index",q.cut.index},{"cut_sha256",q.cut.cut_sha256},{"timing_violation_ps",local},{"captures",std::move(endpoints)}});
        }
        result.covered=true;result.qualified=use==ConditionalUse::Production&&quote.joint_qualified;
        result.area=total;result.violation=violation;result.materialized=std::move(materialized);
        result.report["status"]="covered";result.report["joint_qualified"]=result.qualified;result.report["node_prices"]=std::move(prices);
        result.report["total_area_um2"]=total;result.report["timing_violation_ps"]=violation;result.report["owner_area_um2"]=std::move(owner_json);
        result.report["stages"]=std::move(stages);result.report["state_query"]=*llvm::json::parse(stateCostQueryJson(state));
        result.report["boundary_model_sha256"]=quote.model_sha256;result.report["operator_model_sha256"]=request.model_bundle_sha256;
        if(!quote.state_model_sha256.empty())result.report["state_model_sha256"]=quote.state_model_sha256;
        if(!quote.service_contract_sha256.empty())result.report["service_contract_sha256"]=quote.service_contract_sha256;
        if(!quote.diagnostics_json.empty()) {auto diagnostics=llvm::json::parse(quote.diagnostics_json);if(!diagnostics)fail("invalid boundary diagnostics JSON");result.report["boundary_diagnostics"]=std::move(*diagnostics);}
    } catch(const std::exception& error) {result.report["reason"]=std::string(error.what());}
    return result;
}
}
std::vector<ConditionalBoundaryRequest> conditionalBoundaryRequests(const Program& p,const std::vector<int>& placement,int last,const std::vector<NodeId>& controls) {
    return boundaryRequests(p,placement,last,controls);
}
ConditionalViews enumerateConditionalViews(const Program& source,const pipeline::Options& options,const rtlzz::ArithmeticRequest& policy,std::size_t limit) {
    if(!options.arithmetic_request||options.arithmetic_request->cost_mode!="fixed"||!limit)fail("fixed structural context and finite enumeration limit required");
    ConditionalViews result;result.candidate_policy=policy.candidate_policy;std::set<std::string> ids;
    auto enumeration=visitCandidateGraphs(source,policy,[&](const Program& graph,const std::string& id,const std::string&) {
        auto baseline=pipeline::build(graph,options);const auto& p=baseline.combinational;auto trial=baseline.initial_stages;
        const auto units=pipeline::diagnosticSchedulingUnits(p,options);std::vector<const pipeline::SchedulingUnit*> movable;
        for(const auto& unit:units)if(!unit.constant&&!unit.fixed)movable.push_back(&unit);
        std::set<std::vector<int>> legal;
        std::function<void(std::size_t)> visit=[&](std::size_t n) {
            if(result.assignments>=limit){result.reason="structural assignment limit reached";return;}
            if(n==movable.size()){++result.assignments;if(std::isfinite(pipeline::evaluateSchedule(p,options,trial).final_delay))legal.insert(trial);return;}
            for(int stage=0;stage<=options.register_stages;++stage){for(auto node:movable[n]->members)trial[node]=stage;visit(n+1);if(!result.reason.empty())return;}
        };visit(0);
        std::size_t n=0;for(auto& placement:legal){auto key=id+"/view"+std::to_string(n++);if(!ids.insert(key).second)fail("duplicate structural view id");result.views.push_back({key,id,p,placement});}
    });
    result.structures=enumeration.unique;result.complete=result.reason.empty()&&!result.views.empty();
    if(result.views.empty()&&result.reason.empty())result.reason="no legal structural view";
    result.inventory_sha256=inventoryIdentity(result);
    return result;
}
ConditionalSelection selectConditionalViews(const ConditionalViews& inventory,const pipeline::Options& options,const rtlzz::ArithmeticRequest& request,
                                           const ConditionalBoundaryProvider& boundaries,ConditionalUse use) {
    if(request.latency!=options.register_stages+1||request.initiation_interval!=1)fail("conditional L/II does not match structural placement");
    if(inventory.inventory_sha256!=inventoryIdentity(inventory)||inventory.candidate_policy!=request.candidate_policy)
        fail("conditional structural inventory changed or uses another candidate policy");
    if(use==ConditionalUse::Production)rtlzz::validateArithmeticRequest(request,options.register_stages);
    if(!options.arithmetic_request||options.arithmetic_request->cost_mode!="fixed")fail("conditional materialization requires separate fixed structural context");
    const auto& context=*options.arithmetic_request;
    if(context.target_platform!=request.target_platform||context.platform_profile_sha256!=request.platform_profile_sha256||
       context.operator_boundary_policy!=request.operator_boundary_policy)fail("structural and conditional flow contexts differ");
    auto structural=options;structural.arithmetic_request->target_period_ps=request.target_period_ps;
    std::vector<Evaluation> evaluations;bool complete=inventory.complete,feasible=false;A attempts;std::set<std::string> ids;
    for(const auto& view:inventory.views){if(!ids.insert(view.id).second)fail("duplicate candidate view id");auto value=evaluate(view,structural,request,boundaries,use);
        complete&=value.covered;feasible|=value.covered&&value.violation==0;evaluations.push_back(std::move(value));}
    std::optional<std::tuple<double,double,std::string>> best;std::size_t chosen=0;
    if(complete)for(std::size_t i=0;i<evaluations.size();++i){const auto& v=evaluations[i];if(feasible&&v.violation>0)continue;
        auto score=std::make_tuple(feasible?0.:v.violation,v.area,inventory.views[i].id);if(!best||score<*best){best=score;chosen=i;}}
    ConditionalSelection result;const char* status=!complete?"cost_uncovered":feasible?"predicted_feasible_selected":"target_unmet";
    if(best){result.view_id=inventory.views[chosen].id;result.pipeline=std::move(evaluations[chosen].materialized);}
    for(auto& value:evaluations)attempts.push_back(std::move(value.report));
    O report{{"version","conditional-view-selection-v1"},{"mode",use==ConditionalUse::Production?"production":"TEST_ONLY"},
        {"target_period_ps",request.target_period_ps},{"L",request.latency},{"II",request.initiation_interval},
        {"model_bundle_sha256",request.model_bundle_sha256},{"structural_enumeration_complete",inventory.complete},
        {"candidate_policy",request.candidate_policy},{"structure_limit_K",16},{"global_architecture_optimum_claimed",false},
        {"coverage_complete",complete},{"view_count",std::uint64_t(inventory.views.size())},{"structural_assignments",std::uint64_t(inventory.assignments)},
        {"inventory_sha256",inventory.inventory_sha256},
        {"selection_status",use==ConditionalUse::TestOnly?std::string("TEST_ONLY_")+status:status},
        {"formal_D_selection",bool(best)&&use==ConditionalUse::Production},{"candidate_PPA_calls",0},{"candidates",std::move(attempts)}};
    if(best){report["selected_view"]=result.view_id;report["selected_area_um2"]=evaluations[chosen].area;report["selected_timing_violation_ps"]=evaluations[chosen].violation;}
    if(!inventory.complete)report["enumeration_reason"]=inventory.reason;
    result.report=llvm::formatv("{0:2}",V(std::move(report))).str();return result;
}
}
