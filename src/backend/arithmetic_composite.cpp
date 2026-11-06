#include "backend/arithmetic_composite.hpp"
#include "backend/arithmetic_primitives.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <functional>
#include <set>
#include <sstream>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;
using V=llvm::json::Value;
[[noreturn]] void fail(const std::string& why) {
    throw rtlzz::ArithmeticError({"cost","cost_uncovered",why,{},{}});
}
std::string read(const std::string& path) {
    std::ifstream in(path,std::ios::binary);if(!in)fail("component unavailable: "+path);
    return {std::istreambuf_iterator<char>(in),{}};
}
V load(const std::string& path,const std::string& hash) {
    auto bytes=read(path);if(rtlzz::arithmeticHash(bytes)!=hash)fail("component content identity mismatch: "+path);
    auto value=llvm::json::parse(bytes);
    if(!value){llvm::consumeError(value.takeError());fail("invalid component JSON");}
    if(!value->getAsObject())fail("component object required");return std::move(*value);
}
const O& object(const O& o,const char* key) {auto x=o.getObject(key);if(!x)fail(std::string("missing component object: ")+key);return *x;}
const llvm::json::Array& array(const O& o,const char* key) {auto x=o.getArray(key);if(!x)fail(std::string("missing component array: ")+key);return *x;}
std::string text(const O& o,const char* key) {auto x=o.getString(key);if(!x)fail(std::string("missing component string: ")+key);return x->str();}
double number(const O& o,const char* key) {auto x=o.getNumber(key);if(!x||!std::isfinite(*x))fail(std::string("invalid component number: ")+key);return *x;}
void flag(const O& o,const char* key,bool expected) {auto x=o.getBoolean(key);if(!x||*x!=expected)fail(std::string("component scope flag mismatch: ")+key);}
std::set<int> integers(const O& o,const char* key) {
    std::set<int> result;for(const auto& v:array(o,key)) {
        auto x=v.getAsInteger();if(!x||*x<1||*x>1000000||!result.insert(int(*x)).second)fail("invalid/duplicate exact component domain");
    }return result;
}
void identity(const O& o,const rtlzz::ArithmeticRequest& r) {
    if(text(o,"target_platform")!="asap7"||r.target_platform!="asap7"||
       text(o,"platform_profile_sha256")!=r.platform_profile_sha256||
       number(o,"ABC_ps")!=1500||text(o,"IO")!="0.2T")fail("component platform/profile/ABC/IO mismatch");
}
std::uint64_t count(const std::vector<std::uint64_t>& words) {
    std::uint64_t n=0;for(auto w:words)while(w){w&=w-1;++n;}return n;
}
bool bit(const std::vector<std::uint64_t>& words,int at) {return std::size_t(at/64)<words.size()&&((words[at/64]>>(at%64))&1);}
bool same(const StateSlot& a,const StateSlot& b) {
    return a.origin==b.origin&&a.stage==b.stage&&a.enable_stage==b.enable_stage&&a.declared_width==b.declared_width&&
        a.variable_bits==b.variable_bits&&a.valid==b.valid&&a.external_capture==b.external_capture&&
        a.variable_mask==b.variable_mask&&a.reset_values==b.reset_values;
}
bool registeredReset(const StateSlot& slot) {
    // Interpret patterns on packed variable bits, as the sampled bank does.
    std::vector<bool> bits;for(int b=0;b<slot.declared_width;++b) {
        if(bit(slot.variable_mask,b))bits.push_back(bit(slot.reset_values,b));
        else if(bit(slot.reset_values,b))return false;
    }
    bool zero=true,one=true;for(bool b:bits){zero&=!b;one&=b;}
    if(zero||one)return true;
    for(int modulus:{2,3})for(int phase=0;phase<modulus;++phase) {
        bool matches=true;for(std::size_t b=0;b<bits.size();++b)matches&=bits[b]==((b+phase)%modulus==0);
        if(matches)return true;
    }return false;
}
NodeId channel(const Program& p,NodeId id) {
    std::set<NodeId> seen;
    while(seen.insert(id).second) {
        const auto& d=*p.signal(id).driver;
        if((d.kind!=OperationKind::Assign&&d.kind!=OperationKind::Cast)||d.operands.size()!=1||
           d.operands[0].kind!=OperandKind::Symbol||d.operands[0].type.width!=p.signal(id).type.width)return id;
        id=d.operands[0].node;
    }fail("cyclic native state channel");
}
}

struct NativeStateModel::Impl {
    V document{nullptr};std::string sha;rtlzz::ArithmeticRequest request;
    double bit_coefficient=0,signal_coefficient=0,input_overhead=0,register_overhead=0;
    const O& bundle() const {return *document.getAsObject();}
};
NativeStateModel::NativeStateModel(const std::string& path,const std::string& sha,const rtlzz::ArithmeticRequest& r)
    :impl_(std::make_shared<Impl>()) {
    impl_->document=load(path,sha);impl_->sha=sha;impl_->request=r;const auto& b=impl_->bundle();identity(b,r);
    if(rtlzz::arithmeticHash(read(r.platform_profile))!=r.platform_profile_sha256)fail("native state profile content identity mismatch");
    if(text(b,"version")!=kNativeStateModel||text(b,"quality_status")!="passed"||
       text(b,"feature_schema")!="arithmetic-state-bank-features-v1")fail("native state schema/quality mismatch");
    flag(b,"test_observed",true);flag(b,"independent_labels_consumed",true);
    flag(b,"independent_labels_excluded_from_future_fitting",true);flag(b,"full_D_ready",false);
    if(integers(b,"exact_SDC_ps")!=std::set<int>{375,1500,40500}||
       (r.target_period_ps!=375&&r.target_period_ps!=1500&&r.target_period_ps!=40500))fail("native state exact SDC uncovered");
    const auto& d=object(b,"domain");
    if(integers(d,"word_widths")!=std::set<int>{1,2,4,5,8,16,17,24,32,33,37,48,64,65}||
       number(d,"max_capture_stage")!=3||number(d,"max_parallel_words")!=16||number(d,"max_input_bits")!=640||
       number(d,"max_capture_bits")!=2304||number(d,"max_state_bits")!=2304||number(d,"max_state_signals")!=80||
       text(d,"clock_reset")!="single clock, synchronous active-low reset"||
       text(d,"topology")!="independent scalar channels start at a registered cut and survive for one or more stages; at least one full chain; no arithmetic or formal-case topology copied"||
       text(d,"known_bits")!="all dynamic, high-zero padding, or one rotating known-zero hole; include generic5-bit/four-variable and65-bit/64-variable masks; no known-one constants or dependence between different channels")fail("unsupported native state domain semantics");
    flag(d,"input_service_wrapper_state_included",false);flag(d,"joint_arithmetic_or_formal_boundary_qualified",false);
    flag(d,"continuous_SDC_qualified",false);flag(d,"native_actual_bank_coverage_still_required",true);
    if(text(b,"native_enable_policy")!="single clock, synchronous active-low reset; valid1<=iv; validN<=valid(N-1) always advances; every data capture at stage s enabled by iv at s=1, otherwise valid(s-1); same across all words at a cut")fail("unsupported native state enable policy");
    const auto& stages=object(b,"stages");if(stages.size()!=1)fail("native state release is grt only");
    const auto& grt=object(stages,"grt");if(text(grt,"quality_status")!="passed"||text(object(grt,"independent_quality"),"status")!="passed")fail("native state grt quality not passed");
    const auto& model=object(grt,"area_model");const auto& features=array(model,"features");const auto& coefficients=array(model,"coefficients");
    if(text(model,"kind")!="nonnegative_linear"||features.size()!=2||features[0].getAsString()!="state_bits"||
       features[1].getAsString()!="state_signals"||coefficients.size()!=2)fail("unsupported native state predictor");
    auto a=coefficients[0].getAsNumber(),s=coefficients[1].getAsNumber();
    if(!a||!s||!std::isfinite(*a)||!std::isfinite(*s)||*a<0||*s<0)fail("invalid native state coefficients");
    impl_->bit_coefficient=*a;impl_->signal_coefficient=*s;
    const auto& boundary=object(grt,"boundary_model");
    impl_->input_overhead=number(boundary,"input_overhead_ps");impl_->register_overhead=number(boundary,"register_overhead_ps");
    if(number(boundary,"io_delay_fraction")!=0.2||number(boundary,"engineering_clock_reserve_ps")!=50||
       impl_->input_overhead<50||impl_->register_overhead<50)fail("invalid native state frozen boundary");
    flag(boundary,"input_IO_delay_subtracted_once",true);
}
std::string NativeStateModel::primitiveParent() const {return text(impl_->bundle(),"parent_primitives_sha256");}
std::string NativeStateModel::measurementIdentity() const {return text(impl_->bundle(),"measurement_static_identity_sha256");}
StateCostEstimate NativeStateModel::price(const Program& original,const StateCostQuery& query) const {
    if(query.period_ps!=impl_->request.target_period_ps)fail("native state query/request target mismatch");
    auto expected=stateCostQuery(original,query.projection_options,query.placement,query.period_ps,query.external_outputs_included);
    if(expected.graph_sha256!=query.graph_sha256||expected.layout_sha256!=query.layout_sha256||expected.features!=query.features||
       expected.internal_stages!=query.internal_stages||expected.input_variable_bits!=query.input_variable_bits||
       expected.control_reset_one_bits!=query.control_reset_one_bits||expected.valid_input_rtl_name!=query.valid_input_rtl_name||
       expected.slots.size()!=query.slots.size()||!std::equal(expected.slots.begin(),expected.slots.end(),query.slots.begin(),same))fail("stale or forged native state query");
    const auto& q=expected;const int captures=q.internal_stages+int(q.external_outputs_included);
    if(captures<1||captures>3||q.input_variable_bits>640||q.control_reset_one_bits||q.features.at("state_bits")>2304||
       q.features.at("state_signals")>80)fail("native state bank geometry/control outside qualified domain");
    beir::MutableProgram facts(original);facts.ensureValueFacts();auto p=facts.finish();
    std::map<NodeId,std::vector<const StateSlot*>> chains;std::set<int> valid_stages;
    const auto widths=integers(object(impl_->bundle(),"domain"),"word_widths");
    for(const auto& slot:q.slots) {
        if(slot.stage<1||slot.stage>captures||slot.external_capture!=(q.external_outputs_included&&slot.stage==captures))fail("native state capture ownership mismatch");
        if(slot.valid) {
            if(slot.declared_width!=1||slot.variable_bits!=1||slot.variable_mask!=std::vector<std::uint64_t>{1}||
               slot.reset_values!=std::vector<std::uint64_t>{0}||slot.enable_stage!=-1||!valid_stages.insert(slot.stage).second)fail("native state valid-chain/reset uncovered");
            continue;
        }
        if(!widths.count(slot.declared_width)||!slot.variable_bits||count(slot.variable_mask)!=slot.variable_bits)fail("native state data width/mask uncovered");
        if(slot.enable_stage!=slot.stage-1)fail("native state data enable uncovered");
        if(!registeredReset(slot))fail("native state reset pattern uncovered");
        bool high_padding=true,zero_seen=false;int zeros=0;
        for(int b=0;b<slot.declared_width;++b) {bool dynamic=bit(slot.variable_mask,b);if(!dynamic){++zeros;zero_seen=true;}else if(zero_seen)high_padding=false;}
        if(!high_padding&&zeros!=1)fail("native state known-zero pattern uncovered");
        for(auto w:p.signal(slot.origin).value.known_one)if(w)fail("native state known-one data uncovered");
        chains[channel(p,slot.origin)].push_back(&slot);
    }
    if(int(valid_stages.size())!=captures||chains.empty()||chains.size()>16)fail("native state full valid/channel bank uncovered");
    bool full=false;std::set<NodeId> prior_inputs,valid_inputs;
    for(const auto& port:p.ports)if(port.name==q.projection_options.input_valid_name)
        valid_inputs.insert(port.element_nodes.begin(),port.element_nodes.end());
    for(auto& [origin,slots]:chains) {
        std::sort(slots.begin(),slots.end(),[](auto a,auto b){return a->stage<b->stage;});
        const auto& first=*slots.front();
        for(std::size_t i=0;i<slots.size();++i) {
            const auto& slot=*slots[i];if(slot.stage!=first.stage+int(i)||slot.declared_width!=first.declared_width||
                slot.variable_mask!=first.variable_mask)fail("native state channel overlap/lifetime/shape uncovered");
        }
        full|=first.stage==1&&slots.back()->stage==captures;
        std::set<NodeId> leaves,walked;
        std::function<void(NodeId)> gather=[&](NodeId id) {
            if(!walked.insert(id).second)return;const auto& d=*p.signal(id).driver;
            if(d.kind==OperationKind::PortRead){leaves.insert(id);return;}
            for(const auto& a:d.operands)if(a.kind==OperandKind::Symbol)gather(a.node);
        };gather(origin);
        if(leaves.empty())fail("native state channel lacks independent dynamic source");
        for(auto id:leaves) {
            if(valid_inputs.count(id))fail("native state data depends on valid control");
            if(prior_inputs.count(id))fail("native state dependent channels uncovered");
        }
        prior_inputs.insert(leaves.begin(),leaves.end());
    }
    if(!full)fail("native state bank lacks qualified full channel chain");
    StateCostEstimate result;result.bits=std::uint64_t(q.features.at("state_bits"));result.signals=std::uint64_t(q.features.at("state_signals"));
    result.area_um2=impl_->bit_coefficient*result.bits+impl_->signal_coefficient*result.signals;
    result.input_budget_ps=q.period_ps*(1-0.2)-impl_->input_overhead;
    result.register_budget_ps=q.period_ps-impl_->register_overhead;
    result.model_sha256=impl_->sha;result.layout_sha256=q.layout_sha256;
    if(!std::isfinite(result.area_um2)||!std::isfinite(result.input_budget_ps)||!std::isfinite(result.register_budget_ps))fail("native state prediction overflow");
    return result;
}
StateCostEstimate NativeStateModel::price(const pipeline::Program& p,bool external) const {
    return price(p.combinational,stateCostQuery(p,impl_->request.target_period_ps,external));
}

struct CompositeCostProvider::Impl {
    V document{nullptr};rtlzz::ArithmeticRequest request;
    std::unique_ptr<PublishedPrimitiveCostProvider> primitive;
    std::unique_ptr<NativeStateModel> state;
    std::string primitive_sha,state_sha;
};
CompositeCostProvider::CompositeCostProvider(const Program& p,const rtlzz::ArithmeticRequest& r)
    :CostProvider(costContext(r)),impl_(std::make_shared<Impl>()) {
    impl_->request=r;impl_->document=load(r.model_bundle,r.model_bundle_sha256);const auto& b=*impl_->document.getAsObject();identity(b,r);
    if(text(b,"version")!=kComponentManifest||text(b,"quality_status")!="components_passed"||text(b,"evaluation_stage")!="grt")fail("component manifest schema/quality/stage mismatch");
    flag(b,"full_D_ready",false);flag(b,"joint_arithmetic_boundary_qualified",false);
    const auto& components=object(b,"components");if(components.size()!=2)fail("unsupported component manifest membership");
    auto path=[&](const O& c){auto ref=std::filesystem::path(text(c,"path"));return (ref.is_absolute()?ref:std::filesystem::path(r.model_bundle).parent_path()/ref).lexically_normal().string();};
    const auto& primitive=object(components,"primitives");const auto& state=object(components,"native_state");
    if(text(primitive,"schema")!=kPublishedPrimitiveBundle||text(state,"schema")!=kNativeStateModel)fail("component reference schema mismatch");
    impl_->primitive_sha=text(primitive,"sha256");impl_->state_sha=text(state,"sha256");
    auto child=r;child.model_bundle=path(primitive);child.model_bundle_sha256=impl_->primitive_sha;
    impl_->primitive=std::make_unique<PublishedPrimitiveCostProvider>(p,child);
    auto primitive_document=load(child.model_bundle,child.model_bundle_sha256);
    impl_->state=std::make_unique<NativeStateModel>(path(state),impl_->state_sha,r);
    if(impl_->state->primitiveParent()!=impl_->primitive_sha||impl_->state->measurementIdentity()!=text(*primitive_document.getAsObject(),"measurement_static_identity_sha256"))fail("component parent/physical measurement identity mismatch");
}
std::string CompositeCostProvider::modelIdentity() const {return "asap7-components/"+impl_->request.model_bundle_sha256;}
CostEstimate CompositeCostProvider::node(const Program& p,NodeId id) const {return impl_->primitive->node(p,id);}
CostEstimate CompositeCostProvider::nodeInStage(const Program& p,NodeId id,const StageCostQuery& q) const {return impl_->primitive->nodeInStage(p,id,q);}
StateCostEstimate CompositeCostProvider::stateCost(const Program& p,const StateCostQuery& q) const {return impl_->state->price(p,q);}
std::string CompositeCostProvider::componentReport(const Program& p,const std::vector<int>& placement,int last,const std::vector<NodeId>& controls) const {
    auto report=llvm::json::parse(impl_->primitive->componentReport(p,placement,last,controls));
    if(!report){llvm::consumeError(report.takeError());fail("primitive report JSON");}
    return llvm::formatv("{0:2}",V(O{{"version","component-manifest-coverage-v1"},{"model_id",modelIdentity()},
        {"model_bundle_sha256",impl_->request.model_bundle_sha256},{"primitive_sha256",impl_->primitive_sha},
        {"native_state_sha256",impl_->state_sha},{"state_query_required",true},{"native_state_checked_for_this_placement",false},
        {"pipeline_qualified",false},{"candidate_PPA_calls",0},{"primitive_report",std::move(*report)},
        {"missing_pipeline_components",llvm::json::Array{"joint_arithmetic_and_service_boundary","stage_composition_model","mul_interval_model_when_split"}}})).str();
}
CostEstimate CompositeCostProvider::estimateGroup(const Program&,const std::vector<NodeId>&) const {fail("component_manifest_missing_stage_composition_model");}
double CompositeCostProvider::stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t) const {fail("native_state_requires_typed_query: scalar counts do not qualify a bank");}
void CompositeCostProvider::validateStateProtocol(bool reset,bool valid,bool) const {if(!reset||!valid)fail("native_state_requires_reset_and_valid: actual polarity/reset/enable checked by typed query");}
void CompositeCostProvider::validateInterface(const Program&,const std::string&) const {fail("component_manifest_missing_joint_arithmetic_and_service_boundary_qualification");}
double CompositeCostProvider::dataBudget(bool) const {fail("native_state_boundary_requires_actual_bank_query_and_joint_qualification");}
double CompositeCostProvider::stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>&) const {fail("component_manifest_missing_stage_composition_model");}
double CompositeCostProvider::stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>&) const {fail("component_manifest_missing_stage_composition_model");}
StateCostEstimate CostProvider::stateCost(const Program&,const StateCostQuery&) const {fail("cost provider has no qualified typed state component");}
}
