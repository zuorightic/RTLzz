#include "backend/arithmetic_condition_provider.hpp"
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
[[noreturn]] void fail(const std::string& reason) {throw rtlzz::ArithmeticError({"cost","cost_uncovered",reason,{},{}});}
std::string text(const O& o,const char* key) {auto v=o.getString(key);if(!v)fail(std::string("missing conditional model field: ")+key);return v->str();}
bool wire(const Operation& d) {
    switch(d.kind) {
    case OperationKind::PortRead:case OperationKind::Assign:case OperationKind::Cast:
    case OperationKind::ZExt:case OperationKind::Trunc:case OperationKind::Slice:
    case OperationKind::BitSelect:case OperationKind::Concat:return !d.signed_truncation;
    default:return false;
    }
}
}
struct ConditionalOperatorCostProvider::Impl {
    V document{nullptr};rtlzz::ArithmeticRequest request;std::string graph;
    ConditionalUse use;
};
ConditionalOperatorCostProvider::ConditionalOperatorCostProvider(const Program& p,const rtlzz::ArithmeticRequest& r,ConditionalUse use)
    :CostProvider(costContext(r)),impl_(std::make_shared<Impl>()) {
    impl_->request=r;impl_->graph=rtlzz::arithmeticHash(emitText(p));impl_->use=use;
    if(r.cost_mode!="backend-model"||r.target_platform!="asap7"||r.version!="arithmetic-request-v2"||
       r.operator_boundary_policy!="unsigned-op-hierarchy-v1"||!std::isfinite(r.target_period_ps)||r.target_period_ps<=0)
        fail("conditional model requires an explicit ASAP7 v2 boundary/target request");
    auto bytes=llvm::MemoryBuffer::getFile(r.model_bundle);if(!bytes)fail("conditional model file unavailable");
    if(rtlzz::arithmeticHash((*bytes)->getBuffer().str())!=r.model_bundle_sha256)fail("conditional model content identity mismatch");
    auto parsed=llvm::json::parse((*bytes)->getBuffer());if(!parsed){llvm::consumeError(parsed.takeError());fail("invalid conditional model JSON");}
    impl_->document=std::move(*parsed);auto b=impl_->document.getAsObject();if(!b)fail("conditional model object required");
    if(text(*b,"version")!=kConditionalOperatorModel||text(*b,"profile_sha256")!=r.platform_profile_sha256||
       text(*b,"operator_boundary_policy")!=r.operator_boundary_policy||b->getBoolean("full_D_ready")!=false)
        fail("conditional model schema/profile/boundary/scope mismatch");
    const auto quality=text(*b,"quality_status");
    if(use==ConditionalUse::Production&&(quality!="isolated_functions_passed"||b->getBoolean("test_only").value_or(false)))
        fail("conditional function has not passed independent qualification");
    if(use==ConditionalUse::TestOnly&&quality!="awaiting_independent"&&quality!="isolated_functions_passed")
        fail("TEST_ONLY conditional consumer requires a valid candidate function");
    auto ops=b->getObject("operations");if(!ops||ops->empty())fail("conditional operation inventory missing");
    for(const auto& item:*ops) {
        auto entry=item.second.getAsObject();if(!entry||(item.first!="add"&&item.first!="mul"))fail("unsupported conditional operation inventory");
        auto status=text(*entry,"status");
        if(status!="passed_independent"&&!(use==ConditionalUse::TestOnly&&status=="awaiting_independent"))
            fail("conditional operation is unqualified");
    }
}
std::string ConditionalOperatorCostProvider::modelIdentity() const {return "conditional-operators/"+impl_->request.model_bundle_sha256;}
CostEstimate ConditionalOperatorCostProvider::node(const Program& p,NodeId id) const {
    if(rtlzz::arithmeticHash(emitText(p))!=impl_->graph)fail("conditional node queried with another graph");
    const auto& s=p.signal(id);if(!s.driver)fail("undriven conditional node");
    CostEstimate c;c.delay_unit="ps";c.area_unit="um2";c.state_per_bit=0;c.source=modelIdentity();
    if(s.value.valid&&s.value.constant)return c;
    const auto& d=*s.driver;
    if((d.kind==OperationKind::Binary&&(d.op==OpCode::Mul||d.op==OpCode::Add))||d.kind==OperationKind::AddCarry) {
        c.deferred_to_stage=true;c.reason="conditional price requires actual placement and local consumers";
        c.physical_operation=d.kind==OperationKind::AddCarry?"addcarry":d.op==OpCode::Mul?"mul":"add";return c;
    }
    if(!wire(d)){c.covered=false;c.reason="operation outside conditional Mul/Add/wiring domain";}
    return c;
}
std::vector<ConditionalNodePrice> ConditionalOperatorCostProvider::placementPrices(const Program& p,const std::vector<int>& placement,int last,const std::vector<NodeId>& controls) const {
    if(rtlzz::arithmeticHash(emitText(p))!=impl_->graph)fail("conditional placement queried with another graph");
    const auto& r=impl_->request;auto queries=operatorQueries(p,placement,last,r.target_period_ps,r.platform_profile_sha256,r.operator_boundary_policy,controls);
    std::vector<ConditionalNodePrice> result;std::set<NodeId> seen;
    for(auto& query:queries) {
        if(!seen.insert(query.origin).second)fail("conditional operator appears in more than one stage");
        auto price=operatorConditionEstimate(*impl_->document.getAsObject(),query.descriptor,impl_->use==ConditionalUse::TestOnly);
        result.push_back({std::move(query),std::move(price)});
    }
    return result;
}
CostEstimate ConditionalOperatorCostProvider::nodeInStage(const Program& p,NodeId id,const StageCostQuery& cut) const {
    auto c=node(p,id);if(!c.covered||!c.deferred_to_stage)return c;
    auto actual=stageCostQuery(p,cut.placement,cut.index,cut.last,cut.controls);
    if(actual.graph_sha256!=cut.graph_sha256||actual.cut_sha256!=cut.cut_sha256||actual.members!=cut.members||actual.captures.size()!=cut.captures.size())fail("stale/forged conditional stage context");
    for(std::size_t i=0;i<cut.captures.size();++i)if(cut.captures[i].value!=actual.captures[i].value||cut.captures[i].banks!=actual.captures[i].banks)fail("changed conditional capture context");
    if(std::find(actual.members.begin(),actual.members.end(),id)==actual.members.end())fail("conditional operator is outside queried stage");
    for(const auto& row:placementPrices(p,cut.placement,cut.last,cut.controls))if(row.query.origin==id) {
        if(!row.price.covered){c.covered=false;c.reason=row.price.reason;return c;}
        c.delay=row.price.operator_delay_ps+row.price.delay_reserve_ps;c.combinational=row.price.comb_area_um2;
        c.physical_features=row.query.features;c.physical_features["frozen_delay_reserve_ps"]=row.price.delay_reserve_ps;
        c.deferred_to_stage=false;c.reason.clear();return c;
    }
    c.covered=false;c.reason="no demanded arithmetic query for actual stage member";return c;
}
std::string ConditionalOperatorCostProvider::componentReport(const Program& p,const std::vector<int>& placement,int last,const std::vector<NodeId>& controls) const {
    A rows;unsigned covered=0,uncovered=0;
    if(!placement.empty())for(const auto& row:placementPrices(p,placement,last,controls)) {
        O item{{"node",row.query.origin},{"stage",row.query.stage},{"status",row.price.covered?"covered":"cost_uncovered"},
            {"query",V(O(row.query.descriptor))}};
        if(row.price.covered){++covered;item["operator_delay_ps"]=row.price.operator_delay_ps;item["delay_reserve_ps"]=row.price.delay_reserve_ps;item["comb_area_um2"]=row.price.comb_area_um2;}
        else{++uncovered;item["reason"]=row.price.reason;}rows.push_back(std::move(item));
    }
    return llvm::formatv("{0:2}",V(O{{"version","conditional-operator-coverage-v1"},{"model_id",modelIdentity()},
        {"mode",impl_->use==ConditionalUse::Production?"production_component":"TEST_ONLY"},{"placement_supplied",!placement.empty()},
        {"covered",covered},{"uncovered",uncovered},{"node_prices",std::move(rows)},{"pipeline_qualified",false},
        {"candidate_PPA_calls",0},{"missing_pipeline_components",A{"state_and_shared_prices","joint_stage_boundaries_and_wires"}}})).str();
}
CostEstimate ConditionalOperatorCostProvider::estimateGroup(const Program&,const std::vector<NodeId>&) const {fail("conditional nodes require DAG plus qualified S/shared/boundary composition");}
void ConditionalOperatorCostProvider::validateInterface(const Program&,const std::string&) const {fail("conditional operator functions do not qualify joint S/G/interface timing");}
double ConditionalOperatorCostProvider::stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t) const {fail("conditional operator functions do not price state");}
double ConditionalOperatorCostProvider::dataBudget(bool) const {fail("conditional operator functions lack qualified stage boundaries");}
double ConditionalOperatorCostProvider::stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>&) const {fail("conditional operator functions lack qualified DAG composition");}
double ConditionalOperatorCostProvider::stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>&) const {fail("conditional operator functions lack qualified shared/state area");}
}
