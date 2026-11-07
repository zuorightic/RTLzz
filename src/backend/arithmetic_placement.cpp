#include "backend/arithmetic_placement.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <deque>
#include <iomanip>
#include <set>
#include <sstream>

namespace pred::beir::arithmetic {
std::string generalStateSemanticFingerprint(const GeneralStateCostQuery&);
namespace {
[[noreturn]] void fail(const std::string& reason){throw rtlzz::ArithmeticError({"complete_placement_cost","cost_uncovered",reason,{},{}});}
bool identity(const std::string& s){return s.size()==64&&std::all_of(s.begin(),s.end(),[](char c){return (c>='0'&&c<='9')||(c>='a'&&c<='f');});}
void nonnegative(double v){if(!std::isfinite(v)||v<0)fail("invalid complete placement price quantity");}
llvm::APInt variable(const Signal& s){
    llvm::APInt bits(s.type.width,0);for(int b=0;b<s.type.width;++b){auto k=std::size_t(b/64);auto mask=std::uint64_t(1)<<(b%64);
        bool known=s.value.valid&&((s.value.constant)||(k<s.value.known_zero.size()&&(s.value.known_zero[k]&mask))||(k<s.value.known_one.size()&&(s.value.known_one[k]&mask)));
        if(!known)bits.setBit(b);}return bits;
}
std::vector<unsigned> positions(const llvm::APInt& value){std::vector<unsigned> p;for(unsigned b=0;b<value.getBitWidth();++b)if(value[b])p.push_back(b);return p;}
bool wiring(const Signal& s){if(!s.driver)return false;switch(s.driver->kind){
    case OperationKind::Assign:case OperationKind::Cast:case OperationKind::ZExt:case OperationKind::Trunc:
    case OperationKind::Slice:case OperationKind::Concat:case OperationKind::PortRead:return true;default:return s.value.valid&&s.value.constant;}}
TemplateIntervalCostQuery interval(const Program& p,const ArithmeticRegion& r,int stage,int first,int last){
    TemplateIntervalCostQuery q;q.region=r.id;q.template_id=r.template_version;q.stage=stage;q.first=first;q.last=last;q.parameters=r.template_parameters;
    std::set<NodeId> inside,inputs,outputs;
    for(int k=first;k<=last;++k)inside.insert(r.legal_cut_groups[k].begin(),r.legal_cut_groups[k].end());
    for(auto id:inside)for(const auto& op:p.signal(id).driver->operands)if(op.kind==OperandKind::Symbol&&!inside.count(op.node))inputs.insert(op.node);
    for(const auto& s:p.signals)if(s.driver&&!inside.count(s.id))for(const auto& op:s.driver->operands)if(op.kind==OperandKind::Symbol&&inside.count(op.node))outputs.insert(op.node);
    std::map<NodeId,llvm::APInt> needed;std::deque<NodeId> pending;
    for(auto id:outputs){needed.emplace(id,variable(p.signal(id)));pending.push_back(id);}
    while(!pending.empty()){
        auto id=pending.front();pending.pop_front();if(!inside.count(id))continue;
        const auto& s=p.signal(id);const auto& d=*s.driver;auto mask=needed.at(id)&variable(s);if(mask.isZero())continue;
        std::vector<llvm::APInt> demands;for(const auto& op:d.operands)demands.emplace_back(op.type.width,0);
        if(d.kind==OperationKind::Assign||d.kind==OperationKind::Cast||d.kind==OperationKind::ZExt||d.kind==OperationKind::Trunc)
            demands.at(0)=mask.zextOrTrunc(d.operands[0].type.width);
        else if(d.kind==OperationKind::Slice)demands.at(0)=mask.zextOrTrunc(d.operands[0].type.width).shl(d.lo);
        else if(d.kind==OperationKind::Concat){unsigned offset=0;for(std::size_t k=d.operands.size();k-->0;){demands[k]=mask.lshr(offset).zextOrTrunc(d.operands[k].type.width);offset+=d.operands[k].type.width;}}
        else if(d.kind==OperationKind::AddCarry||(d.kind==OperationKind::Binary&&d.op==OpCode::Mul)){
            unsigned bits=mask.getActiveBits();for(std::size_t k=0;k<demands.size();++k)demands[k]=llvm::APInt::getLowBitsSet(d.operands[k].type.width,std::min(bits,unsigned(d.operands[k].type.width)));
        }else fail("unsupported typed interval bit dependency");
        for(std::size_t k=0;k<d.operands.size();++k)if(d.operands[k].kind==OperandKind::Symbol){auto child=d.operands[k].node;auto value=demands[k]&variable(p.signal(child));
            auto at=needed.find(child);if(at==needed.end()){needed.emplace(child,value);if(!value.isZero())pending.push_back(child);}
            else if((at->second|value)!=at->second){at->second|=value;pending.push_back(child);}}
    }
    for(auto id:inside)q.members.push_back(id);
    for(auto id:inputs){auto at=needed.find(id);if(at==needed.end())continue;auto bits=positions(variable(p.signal(id))&at->second);
        if(!bits.empty()){q.inputs.push_back(id);q.input_bit_positions.emplace(id,std::move(bits));}}
    for(auto id:outputs){auto bits=positions(variable(p.signal(id)));if(!bits.empty()){q.outputs.push_back(id);q.output_bit_positions.emplace(id,std::move(bits));}}
    if(q.inputs.empty()||q.outputs.empty())fail("interval lacks observable variable boundary");
    std::ostringstream h;h<<rtlzz::arithmeticHash(emitText(p))<<'/'<<r.id<<'/'<<stage<<'/'<<first<<'/'<<last;
    for(const auto& [id,bits]:q.input_bit_positions){h<<"/in:"<<id;for(auto b:bits)h<<':'<<b;}
    for(const auto& [id,bits]:q.output_bit_positions){h<<"/out:"<<id;for(auto b:bits)h<<':'<<b;}
    q.query_sha256=rtlzz::arithmeticHash(h.str());return q;
}
CompletePlacementCostQuery assemble(const Program& p,GeneralStateCostQuery state){
    CompletePlacementCostQuery q;q.state=std::move(state);auto placement=q.state.core.placement;int n=q.state.core.internal_stages;
    const auto& options=q.state.core.projection_options;std::vector<NodeId> controls;
    for(const auto& port:p.ports)if(port.name==options.input_valid_name)controls.push_back(port.element_nodes.at(0));
    for(const auto& port:p.ports)if(port.name==options.output_valid_name)controls.push_back(port.element_nodes.at(0));
    if(controls.size()!=2)fail("complete placement requires explicit valid protocol ports");
    for(int at=0;at<=n;++at)q.stages.push_back(stageCostQuery(p,placement,at,n,controls));
    for(int at=0;at<=n;++at){CompletePlacementCostQuery::StorageBoundary boundary;
        std::ostringstream h;h<<q.state.layout_sha256<<'/'<<at;
        for(std::size_t k=0;k<q.state.slots.size();++k){const auto& slot=q.state.slots[k];if(slot.state.valid||!slot.state.variable_bits)continue;
            if(slot.state.stage==at&&slot.owner==(at==0?"input_service":"native_state")){boundary.launch_slots.push_back(k);h<<"/Q:"<<k;}
            if(slot.state.stage==at+1&&slot.owner==(at==n?"external_state":"native_state")){
                boundary.capture_slots.push_back(k);h<<"/D:"<<k;
                if(placement.at(slot.state.origin)<at){boundary.live_through_capture_slots.push_back(k);h<<"/through:"<<k;}}
        }boundary.sha256=rtlzz::arithmeticHash(h.str());q.storage_boundaries.push_back(std::move(boundary));}
    for(const auto& r:p.arithmetic_regions){
        if(!r.internal_split_enabled){int where=-1;for(auto id:r.members)if(!(p.signal(id).value.valid&&p.signal(id).value.constant)){
            if(where<0)where=placement.at(id);else if(where!=placement.at(id))fail("complete placement cut violates atomic region");}continue;}
        if(r.template_version!="mul-block-tree-v1"||r.legal_cut_groups.empty())fail("complete placement lacks registered template groups");
        std::map<int,std::vector<int>> groups;
        for(std::size_t k=0;k<r.legal_cut_groups.size();++k){int at=-1;for(auto id:r.legal_cut_groups[k]){
            if(at<0)at=placement.at(id);else if(at!=placement.at(id))fail("complete placement split an indivisible template group");}
            if(at<0)fail("empty legal template group");groups[at].push_back(k);}
        for(auto [at,set]:groups){if(set.back()-set.front()+1!=int(set.size()))fail("noncontiguous template groups at one stage need a different interval model");
            q.intervals.push_back(interval(p,r,at,set.front(),set.back()));}
    }
    std::ostringstream h;h<<q.state.layout_sha256;for(const auto& s:q.stages)h<<'/'<<s.cut_sha256;for(const auto& b:q.storage_boundaries)h<<'/'<<b.sha256;for(const auto& i:q.intervals)h<<'/'<<i.query_sha256;
    if(options.arithmetic_request){const auto& r=*options.arithmetic_request;h<<"/model:"<<r.model_bundle_sha256<<"/profile:"<<r.platform_profile_sha256<<"/platform:"<<r.target_platform;}
    q.query_sha256=rtlzz::arithmeticHash(h.str());return q;
}
}
CompletePlacementCostQuery completePlacementCostQuery(const Program& p,const pipeline::Options& o,const std::vector<int>& place,double t,const ServiceStateProtocol& service){
    return assemble(p,generalStateCostQuery(p,o,place,t,service));
}
CompletePlacementCostQuery completePlacementCostQuery(const pipeline::Program& p,double t,const ServiceStateProtocol& service){
    return assemble(p.combinational,generalStateCostQuery(p,t,service));
}
PartialPlacementCostQuery partialPlacementCostQuery(const Program& p,const pipeline::Options& o,const std::vector<int>& placement,double t,const ServiceStateProtocol& service){
    pipeline::validateOptions(o);
    if(placement.size()!=p.signals.size()||!std::isfinite(t)||t<=0)fail("invalid partial placement shape/clock");
    for(const auto& s:p.signals){int at=placement.at(s.id);if(at< -1||at>o.register_stages)fail("partial stage outside declared latency");
        if(at>=0&&s.driver)for(const auto& op:s.driver->operands)if(op.kind==OperandKind::Symbol&&placement.at(op.node)>at)fail("partial assignment reverses dependency");}
    PartialPlacementCostQuery q;q.placement=placement;q.options=o;q.period_ps=t;q.service=service;q.graph_sha256=rtlzz::arithmeticHash(emitText(p));
    std::ostringstream h;h<<std::setprecision(17)<<q.graph_sha256<<'/'<<t<<'/'<<o.register_stages<<'/'<<o.input_valid_name<<'/'<<o.output_valid_name<<'/'<<service.mode;
    h<<'/'<<o.reset_enabled<<'/'<<o.reset_active_low<<'/'<<o.reset_name<<'/'<<o.clock_name;
    if(o.arithmetic_request){const auto& r=*o.arithmetic_request;h<<"/model:"<<r.model_bundle_sha256<<"/profile:"<<r.platform_profile_sha256<<"/platform:"<<r.target_platform;}
    for(int at:placement)h<<'/'<<at;for(const auto& [port,en]:service.data_write_enables)h<<'/'<<port<<':'<<en;
    h<<'/'<<service.valid_write_enable<<'/'<<service.external_data_unconditional<<'/'<<service.external_valid_unconditional;
    for(const auto& [port,words]:o.input_reset_values){h<<'/'<<port;for(auto w:words)h<<':'<<w;}
    q.query_sha256=rtlzz::arithmeticHash(h.str());return q;
}
CompletePlacementEvaluation evaluateCompletePlacement(const Program& p,const CompletePlacementCostQuery& supplied,const CostProvider& provider){
    const auto& c=supplied.state.core;auto q=completePlacementCostQuery(p,c.projection_options,c.placement,c.period_ps,supplied.state.protocol);
    if(generalStateSemanticFingerprint(supplied.state)!=q.state.layout_sha256||supplied.state.core.features!=q.state.core.features||
        supplied.state.core.slots.size()!=q.state.core.slots.size())
        fail("forged complete placement state owners/features");
    if(q.query_sha256!=supplied.query_sha256||completePlacementCostQueryJson(q)!=completePlacementCostQueryJson(supplied)){
        // Materialized evidence changes native names only; normalize by using a
        // projected query as the numerical input and validate the final reload separately.
        auto normalized=supplied;normalized.state=q.state;
        if(q.query_sha256!=supplied.query_sha256||completePlacementCostQueryJson(q)!=completePlacementCostQueryJson(normalized))fail("stale or forged complete placement query");
        if(supplied.state.layout_sha256!=q.state.layout_sha256)fail("stale complete state layout");
    }
    if(!provider.requiresCompletePlacementCost())fail("provider has no qualified complete-placement model");
    CompletePlacementEvaluation result;result.price=provider.completePlacementCost(p,q);auto& price=result.price;
    if(price.query_sha256!=q.query_sha256||!identity(price.model_sha256)||price.model_sha256!=provider.context().model_bundle_sha256||
        !price.joint_source_and_boundary_qualified||price.stages.size()!=q.stages.size())
        fail("complete-placement model identity/source/boundary coverage absent");
    result.feasible=true;std::set<int> stages;
    for(const auto& s:price.stages){
        if(s.index<0||s.index>=int(q.stages.size())||!stages.insert(s.index).second||s.cut_sha256!=q.stages[s.index].cut_sha256||
            s.state_boundary_sha256!=q.storage_boundaries[s.index].sha256||!identity(s.model_sha256))fail("wrong/duplicate stage price owner or cut/storage boundary");
        nonnegative(s.QD_ps);nonnegative(s.clock_capture_overhead_ps);nonnegative(s.combinational_area_um2);
        if(s.ownership!="whole_stage"&&s.ownership!="interval_stage")fail("invalid complete stage ownership mode");
        auto paid=s.owned_members;std::sort(paid.begin(),paid.end());if(paid!=q.stages[s.index].members)fail("stage logic not accounted exactly once");
        std::set<NodeId> interval_members;std::set<std::string> expected;
        for(const auto& i:q.intervals)if(i.stage==s.index){expected.insert(i.query_sha256);interval_members.insert(i.members.begin(),i.members.end());
            auto at=s.interval_qualification_sha256.find(i.query_sha256);if(at==s.interval_qualification_sha256.end()||!identity(at->second))fail("real interval qualification missing from stage price");}
        if(expected.size()!=s.interval_qualification_sha256.size())fail("foreign/duplicate interval ownership receipt");
        if(s.ownership=="interval_stage")for(auto id:s.owned_members)if(!interval_members.count(id)&&!wiring(p.signal(id)))fail("ordinary arithmetic cannot be billed as an interval");
        double budget=c.period_ps-s.clock_capture_overhead_ps;result.stage_budgets_ps.resize(q.stages.size());result.stage_delays_ps.resize(q.stages.size());
        result.stage_budgets_ps[s.index]=budget;result.stage_delays_ps[s.index]=s.QD_ps;
        result.feasible&=budget>=0&&s.QD_ps<=budget;result.combinational_area_um2+=s.combinational_area_um2;result.maximum_stage_delay_ps=std::max(result.maximum_stage_delay_ps,s.QD_ps);
    }
    if(price.functional_state_area_um2.size()!=3||price.state_owner_model_sha256.size()!=3)fail("incomplete functional state ownership");
    for(const auto& owner:{"input_service","native_state","external_state"}){
        auto area=price.functional_state_area_um2.find(owner);auto model=price.state_owner_model_sha256.find(owner);
        if(area==price.functional_state_area_um2.end()||model==price.state_owner_model_sha256.end()||!identity(model->second))fail("unqualified functional state owner");
        nonnegative(area->second);result.functional_state_area_um2+=area->second;
    }
    if(!identity(price.shared_physical_model_sha256))fail("shared clock/physical/root repair owner unqualified");
    nonnegative(price.shared_physical_area_um2);
    const std::set<std::string> boundaries{"native_enable","native_valid","external_data","external_valid","input_data","input_enable","input_valid"};
    if(price.control_boundary_upper_ps.size()!=boundaries.size())fail("complete control/host boundary coverage incomplete");
    for(const auto& path:boundaries){auto at=price.control_boundary_upper_ps.find(path);if(at==price.control_boundary_upper_ps.end())fail("missing state/control boundary");nonnegative(at->second);
        double margin=c.period_ps*(path.rfind("input_",0)==0?.8:1.)-at->second;result.boundary_margins_ps[path]=margin;result.feasible&=margin>=0;}
    for(const auto& slot:q.state.slots){result.all_variable_state_bits+=slot.state.variable_bits;result.all_state_signals+=slot.state.variable_bits>0;
        if(slot.owner=="native_state"){result.native_declared_bits+=slot.state.declared_width;++result.native_register_count;}}
    result.total_area_um2=result.combinational_area_um2+result.functional_state_area_um2+price.shared_physical_area_um2;nonnegative(result.total_area_um2);
    return result;
}
CompletePlacementEvaluation evaluateCompletePlacement(const pipeline::Program& p,double period,const ServiceStateProtocol& service,const CostProvider& provider){
    auto actual=completePlacementCostQuery(p,period,service);return evaluateCompletePlacement(p.combinational,actual,provider);
}
PartialPlacementBound evaluatePartialPlacement(const Program& p,const PartialPlacementCostQuery& supplied,const CostProvider& provider){
    auto q=partialPlacementCostQuery(p,supplied.options,supplied.placement,supplied.period_ps,supplied.service);
    if(q.query_sha256!=supplied.query_sha256)fail("stale partial placement bound request");
    auto bound=provider.partialPlacementBound(p,q);
    if(!bound.certified){if(bound.maximum_stage_delay_lower_ps||bound.total_area_lower_um2)fail("unknown partial bound must not contain a zero/substitute price");return bound;}
    if(bound.query_sha256!=q.query_sha256||!identity(bound.model_sha256)||bound.model_sha256!=provider.context().model_bundle_sha256||
        (!bound.maximum_stage_delay_lower_ps&&!bound.total_area_lower_um2))fail("partial lower bound lacks qualified identity");
    if(bound.maximum_stage_delay_lower_ps)nonnegative(*bound.maximum_stage_delay_lower_ps);
    if(bound.total_area_lower_um2)nonnegative(*bound.total_area_lower_um2);return bound;
}
CompletePlacementPrice CostProvider::completePlacementCost(const Program&,const CompletePlacementCostQuery&) const{fail("complete G/I/state/shared model package not qualified");}
PartialPlacementBound CostProvider::partialPlacementBound(const Program&,const PartialPlacementCostQuery&) const{
    PartialPlacementBound b;b.reason="no certified partial-stage lower bound";return b;
}
std::string completePlacementCostQueryJson(const CompletePlacementCostQuery& q){
    using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
    auto ids=[](const auto& values){A a;for(auto v:values)a.push_back(v);return a;};A stages,intervals,boundaries;
    for(const auto& s:q.stages){A captures;for(const auto& c:s.captures)captures.push_back(O{{"value",c.value},{"banks",c.banks}});
        stages.push_back(O{{"index",s.index},{"cut_sha256",s.cut_sha256},{"members",ids(s.members)},
            {"controls",ids(s.controls)},{"placement",ids(s.placement)},{"captures",std::move(captures)}});}
    for(const auto& i:q.intervals){O inputs,outputs,parameters;for(const auto& [n,b]:i.input_bit_positions)inputs[std::to_string(n)]=ids(b);for(const auto& [n,b]:i.output_bit_positions)outputs[std::to_string(n)]=ids(b);
        for(const auto& [k,v]:i.parameters)parameters[k]=v;
        intervals.push_back(O{{"stage",i.stage},{"region",i.region},{"template_id",i.template_id},{"first",i.first},{"last",i.last},
            {"members",ids(i.members)},{"inputs",ids(i.inputs)},{"outputs",ids(i.outputs)},{"input_bit_positions",std::move(inputs)},{"output_bit_positions",std::move(outputs)},
            {"parameters",std::move(parameters)},{"query_sha256",i.query_sha256}});}
    for(const auto& b:q.storage_boundaries)boundaries.push_back(O{{"sha256",b.sha256},{"launch_slots",ids(b.launch_slots)},
        {"capture_slots",ids(b.capture_slots)},{"live_through_capture_slots",ids(b.live_through_capture_slots)}});
    auto state=llvm::json::parse(generalStateCostQueryJson(q.state));if(!state){llvm::consumeError(state.takeError());fail("complete state JSON");}
    return llvm::formatv("{0:2}",V(O{{"version","complete-placement-query-v1"},{"query_sha256",q.query_sha256},{"state",std::move(*state)},
        {"stages",std::move(stages)},{"storage_boundaries",std::move(boundaries)},{"intervals",std::move(intervals)},{"PPA_calls",0}})).str();
}
std::string completePlacementEvaluationJson(const CompletePlacementEvaluation& e){
    using O=llvm::json::Object;using A=llvm::json::Array;A budgets,delays,prices;O margins;
    for(auto v:e.stage_budgets_ps)budgets.push_back(v);for(auto v:e.stage_delays_ps)delays.push_back(v);for(const auto& [p,v]:e.boundary_margins_ps)margins[p]=v;
    for(const auto& s:e.price.stages)prices.push_back(O{{"stage",s.index},{"domain",s.qualified_domain},{"model_sha256",s.model_sha256},
        {"cut_sha256",s.cut_sha256},{"state_boundary_sha256",s.state_boundary_sha256},{"ownership",s.ownership},
        {"QD_ps",s.QD_ps},{"clock_capture_overhead_ps",s.clock_capture_overhead_ps},{"combinational_area_um2",s.combinational_area_um2}});
    return llvm::formatv("{0:2}",llvm::json::Value(O{{"query_sha256",e.price.query_sha256},{"stage_budgets_ps",std::move(budgets)},
        {"stage_delays_ps",std::move(delays)},{"stage_prices",std::move(prices)},{"boundary_margins_ps",std::move(margins)},{"combinational_area_um2",e.combinational_area_um2},
        {"functional_state_area_um2",e.functional_state_area_um2},{"shared_physical_area_um2",e.price.shared_physical_area_um2},
        {"total_area_um2",e.total_area_um2},{"maximum_stage_delay_ps",e.maximum_stage_delay_ps},{"feasible",e.feasible},
        {"all_variable_state_bits",e.all_variable_state_bits},{"native_declared_bits",e.native_declared_bits},{"PPA_calls",0}})).str();
}
}
