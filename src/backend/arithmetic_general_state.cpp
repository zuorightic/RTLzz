#include "backend/arithmetic_general_state.hpp"
#include "backend/arithmetic_primitives.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <set>

namespace pred::beir::arithmetic {
std::string generalStateSemanticFingerprint(const GeneralStateCostQuery&);
namespace {
using O=llvm::json::Object;using V=llvm::json::Value;
[[noreturn]] void fail(const std::string& reason){throw rtlzz::ArithmeticError({"general_state_cost","cost_uncovered",reason,{},{}});}
std::string read(const std::string& path){std::ifstream f(path,std::ios::binary);if(!f)fail("general state component unavailable: "+path);return {std::istreambuf_iterator<char>(f),{}};}
V load(const std::string& path,const std::string& hash){auto bytes=read(path);if(rtlzz::arithmeticHash(bytes)!=hash)fail("general state component hash mismatch");
    auto v=llvm::json::parse(bytes);if(!v){llvm::consumeError(v.takeError());fail("general state component JSON invalid");}if(!v->getAsObject())fail("general state component object required");return std::move(*v);}
const O& object(const O& o,const char* name){auto p=o.getObject(name);if(!p)fail(std::string("general state missing object: ")+name);return *p;}
const llvm::json::Array& array(const O& o,const char* name){auto p=o.getArray(name);if(!p)fail(std::string("general state missing array: ")+name);return *p;}
std::string text(const O& o,const char* name){auto p=o.getString(name);if(!p)fail(std::string("general state missing string: ")+name);return p->str();}
double num(const O& o,const char* name){auto p=o.getNumber(name);if(!p||!std::isfinite(*p))fail(std::string("general state nonfinite/missing number: ")+name);return *p;}
void flag(const O& o,const char* name,bool expected){auto p=o.getBoolean(name);if(!p||*p!=expected)fail(std::string("general state flag mismatch: ")+name);}
std::set<int> ints(const O& o,const char* name){std::set<int> result;for(const auto& v:array(o,name)){auto n=v.getAsInteger();if(!n||*n<1||*n>100000||!result.insert(int(*n)).second)fail("invalid general state discrete domain");}return result;}
std::vector<std::string> strings(const O& o,const char* name){std::vector<std::string> a;for(const auto& v:array(o,name)){auto s=v.getAsString();if(!s)fail("invalid general state string list");a.push_back(s->str());}return a;}
void identity(const O& b,const rtlzz::ArithmeticRequest& r){if(r.target_platform!="asap7"||text(b,"target_platform")!="asap7"||
    text(b,"platform_profile_sha256")!=r.platform_profile_sha256||rtlzz::arithmeticHash(read(r.platform_profile))!=r.platform_profile_sha256||
    num(b,"ABC_ps")!=1500||text(b,"IO")!="0.2T")fail("general state platform/profile/ABC/IO mismatch");}
const std::set<int> clocks{1119,1253,1431,3141,3385,4162,4528,11913,14897};
const std::set<int> widths{1,5,13,16,17,21,22,24,25,26,27,29,30,32,33,34,35,37,40,41,42,48,49,50,56,57,58,64,65,66};
const std::vector<std::string> controls{"one","native_enable_bits_max","native_enable_bits_sqrt","native_enable_bits_log2","native_enable_words_max",
    "native_valid_targets_max","native_data_bits","native_reset_one_bits","input_data_bits","external_data_bits"};
const std::vector<std::string> area_features{"data_bits","data_signals","valid_bits","enable_domains","reset_one_bits"};
const std::vector<std::string> fixed_paths{"native_data","external_data","external_valid","input_data","input_enable","input_valid"};
double linear(const O& model,const std::map<std::string,double>& f,const std::vector<std::string>& names){
    if(text(model,"kind")!="nonnegative_linear"||strings(model,"features")!=names||array(model,"coefficients").size()!=names.size())fail("unsupported general state linear predictor/schema");
    double value=0;for(std::size_t i=0;i<names.size();++i){auto c=array(model,"coefficients")[i].getAsNumber();auto it=f.find(names[i]);
        if(!c||!std::isfinite(*c)||*c<0||it==f.end()||!std::isfinite(it->second)||it->second<0)fail("invalid general state feature/coefficient");value+=*c*it->second;}
    if(!std::isfinite(value)||value<0)fail("general state linear overflow");return value;
}
bool bit(const std::vector<std::uint64_t>& a,int b){return std::size_t(b/64)<a.size()&&((a[b/64]>>(b%64))&1);}
}
struct GeneralStateModel::Impl {
    V document{nullptr};std::string sha;rtlzz::ArithmeticRequest request;
    const O& bundle() const{return *document.getAsObject();}
    const O& stage() const{return object(object(bundle(),"stages"),"grt");}
};
GeneralStateModel::GeneralStateModel(const std::string& path,const std::string& hash,const rtlzz::ArithmeticRequest& r):impl_(std::make_shared<Impl>()){
    impl_->document=load(path,hash);impl_->sha=hash;impl_->request=r;const auto& b=impl_->bundle();identity(b,r);
    if(text(b,"version")!=kGeneralStateModel||text(b,"quality_status")!="passed"||ints(b,"exact_SDC_ps")!=clocks||
       !std::isfinite(r.target_period_ps)||!std::any_of(clocks.begin(),clocks.end(),[&](int t){return r.target_period_ps==t;}))fail("general state schema/quality/exact clock uncovered");
    for(const auto& key:{"test_observed","independent_labels_consumed","independent_labels_excluded_from_future_fitting","area_models_and_six_path_bounds_unchanged"})flag(b,key,true);
    for(const auto& key:{"full_D_ready","production_price_binding_qualified","arbitrary_arithmetic_source_correlation_qualified","shared_clock_physical_repair_prices_included"})flag(b,key,false);
    const auto& d=object(b,"domain");
    if(ints(d,"word_widths")!=widths||num(d,"max_variable_word_bits")!=64||num(d,"max_parallel_words")!=18||num(d,"max_input_data_bits")!=130||
        num(d,"max_native_data_bits")!=1024||num(d,"max_external_data_bits")!=256||ints(d,"native_depths")!=std::set<int>{1,2}||
        text(d,"reset")!="synchronous active-low; per-channel zero/ones/alternating/hash-derived general bit patterns; same logical value reset along its lifetime"||
        text(d,"source_relations")!="kept combinational two-input XOR/permutation/inversion adapters over shared registered roots; every semantic state bit observed exactly once; adapter arithmetic excluded from owned state area"||
        text(d,"relation_limit")!="synthetic affine two-root-bit relations only; arbitrary arithmetic source correlation still requires joint G qualification"||
        text(d,"external_protocol")!="unconditional output data/valid capture from declared terminal native values; never gated by valid")fail("unsupported/extended general state domain semantics");
    const auto& stages=object(b,"stages");if(stages.size()!=1||text(impl_->stage(),"quality_status")!="passed"||text(object(impl_->stage(),"independent_quality"),"status")!="passed")fail("general state qualified grt release required");
    const auto& query=object(b,"query_contract");if(strings(query,"control_features")!=controls)fail("general state control feature schema mismatch");
    std::map<std::string,double> dummy;for(const auto& name:area_features)dummy[name]=0;
    const auto& areas=object(impl_->stage(),"area_models");if(areas.size()!=3)fail("general state area ownership incomplete");
    for(const auto& owner:{"input_service","native_state","external_state"})linear(object(areas,owner),dummy,area_features);
    const auto& boundary=object(impl_->stage(),"boundary_model");
    if(text(boundary,"kind")!="fixed_six_paths_and_semantic_control_upper"||strings(boundary,"mutable_paths")!=std::vector<std::string>{"native_enable","native_valid"})fail("unsupported general state boundary family");
    flag(boundary,"birth_data_diagnostic_only",true);flag(boundary,"arbitrary_joint_arithmetic_boundary_qualified",false);
    const auto& fixed=object(boundary,"immutable_by_exact_SDC");if(fixed.size()!=clocks.size())fail("general state fixed clock membership differs");
    for(int t:clocks){const auto& paths=object(fixed,std::to_string(t).c_str());if(paths.size()!=fixed_paths.size())fail("general state fixed path membership differs");
        for(const auto& path:fixed_paths){const auto& v=object(paths,path.c_str());if(num(v,"reserve_ps")!=50||num(v,"overhead_ps")<50)fail("invalid general state fixed reserve/bound");}}
    const auto& models=object(boundary,"control_models");if(models.size()!=2)fail("general state controls incomplete");
    auto names=controls;for(int t:clocks)names.push_back("exact_SDC_"+std::to_string(t));
    for(const auto& key:{"native_enable","native_valid"}){const auto& model=object(models,key);
        if(text(model,"kind")!="train_NNLS_semantic_control_load_exact_clock"||num(model,"reserve_ps")!=50||num(model,"train_upper_correction_ps")<0||
            strings(model,"calibration_splits")!=std::vector<std::string>{"train"})fail("unsupported general state control correction/family");
        std::map<std::string,double> f;for(const auto& n:names)f[n]=0;linear(object(model,"regression"),f,names);
    }
}
std::string GeneralStateModel::measurementIdentity() const{return text(impl_->bundle(),"measurement_static_identity_sha256");}
std::string GeneralStateModel::nativeParent() const{return text(impl_->bundle(),"parent_model_sha256");}
GeneralStateCostEstimate GeneralStateModel::price(const Program& p,const GeneralStateCostQuery& supplied) const {
    if(supplied.core.period_ps!=impl_->request.target_period_ps)fail("general state query/request clock mismatch");
    auto q=generalStateCostQuery(p,supplied.core.projection_options,supplied.core.placement,supplied.core.period_ps,supplied.protocol);
    if(q.layout_sha256!=supplied.layout_sha256||generalStateSemanticFingerprint(supplied)!=q.layout_sha256||
       supplied.core.features!=q.core.features||supplied.core.slots.size()!=q.core.slots.size())fail("stale or forged general state query");
    if(q.core.internal_stages<1||q.core.internal_stages>2||q.core.control_reset_one_bits)fail("general state native depth/valid reset uncovered");
    const auto& input=q.owner_features.at("input_service");const auto& native=q.owner_features.at("native_state");const auto& external=q.owner_features.at("external_state");
    // Consumer narrows the published capacity envelope to the actually sampled
    // service root load. All S source roots total130 bits (two or three words).
    // G qualification may later admit different surrounding service loads.
    if(input.at("data_bits")!=130||(input.at("data_signals")!=2&&input.at("data_signals")!=3)||
       native.at("data_bits")>1024||external.at("data_bits")>256||external.at("data_bits")<1||
       native.at("data_bits")<1||native.at("valid_bits")!=q.core.internal_stages||input.at("valid_bits")!=1||external.at("valid_bits")!=1)
        fail("general state bank/service load outside sampled conservative consumer domain");
    if(!q.source_scope_qualified)fail("general state source relation requires joint_G_qualification");
    std::set<NodeId> channels;std::map<NodeId,std::vector<const StateSlot*>> chains;
    std::set<std::pair<int,std::string>> unique,unique_bits;
    for(const auto& item:q.slots){const auto& slot=item.state;
        if(slot.valid){for(auto word:slot.reset_values)if(word)fail("general state valid reset must be zero");continue;}
        if(item.owner=="input_service")continue;
        if(!widths.count(slot.declared_width)||slot.variable_bits<1||slot.variable_bits>64)fail("general state word width/variable geometry uncovered: owner="+item.owner+" origin="+std::to_string(slot.origin)+" width="+std::to_string(slot.declared_width)+" variable="+std::to_string(slot.variable_bits));
        for(int b=0;b<slot.declared_width;++b)if(!bit(slot.variable_mask,b)&&bit(slot.reset_values,b))fail("general state known-one padding uncovered");
        if(item.owner=="native_state"){
            if(slot.enable_stage!=slot.stage-1)fail("general state native data must use previous valid");
            channels.insert(slot.origin);chains[slot.origin].push_back(&slot);
        }else if(slot.enable_stage!=-1)fail("general state external capture must be unconditional");
        // Equal functions at the same cut can be optimized into shared FFs.
        // Declared distinct channel IDs cannot qualify duplicate storage.
        std::string signature;for(const auto& term:item.affine_bits)signature+=term+';';
        if(!unique.emplace(slot.stage,signature).second)fail("general state duplicate/aliased storage at one cut uncovered");
        for(const auto& term:item.affine_bits)if(!term.empty()){
            if(!unique_bits.emplace(slot.stage,term.substr(1)).second)fail("general state repeated affine bit storage at one cut uncovered");
        }
    }
    if(channels.empty()||channels.size()>18)fail("general state native channel capacity uncovered");
    for(auto& [id,slots]:chains){std::sort(slots.begin(),slots.end(),[](auto a,auto b){return a->stage<b->stage;});
        for(std::size_t i=1;i<slots.size();++i)if(slots[i]->stage!=slots[i-1]->stage+1||slots[i]->reset_values!=slots[0]->reset_values||
            slots[i]->variable_mask!=slots[0]->variable_mask)fail("general state channel reset/mask/lifetime mismatch");}
    GeneralStateCostEstimate result;result.model_sha256=impl_->sha;result.layout_sha256=q.layout_sha256;
    const auto& area=object(impl_->stage(),"area_models");
    for(const auto& [owner,f]:q.owner_features){double value=linear(object(area,owner.c_str()),f,area_features);
        result.owner_area_um2[owner]=value;result.owned_state_area_um2+=value;
        result.bits+=std::uint64_t(f.at("state_bits"));result.signals+=std::uint64_t(f.at("state_signals"));}
    const auto& boundary=object(impl_->stage(),"boundary_model");const auto& fixed=object(object(boundary,"immutable_by_exact_SDC"),std::to_string(int(q.core.period_ps)).c_str());
    for(const auto& path:fixed_paths)result.boundary_upper_ps[path]=num(object(fixed,path.c_str()),"overhead_ps");
    auto f=q.control_features;auto names=controls;for(int t:clocks){auto name="exact_SDC_"+std::to_string(t);f[name]=t==q.core.period_ps;names.push_back(name);}
    for(const auto& path:{"native_enable","native_valid"}){const auto& model=object(object(boundary,"control_models"),path);
        result.boundary_upper_ps[path]=linear(object(model,"regression"),f,names)+num(model,"train_upper_correction_ps")+num(model,"reserve_ps");}
    for(auto [path,value]:result.boundary_upper_ps){double budget=q.core.period_ps*(path.rfind("input_",0)==0?.8:1.)-value;
        if(!std::isfinite(budget))fail("general state conditional residual overflow");result.residual_budget_ps[path]=budget;}
    return result;
}
GeneralStateCostEstimate GeneralStateModel::price(const pipeline::Program& p,const ServiceStateProtocol& protocol) const{
    return price(p.combinational,generalStateCostQuery(p,impl_->request.target_period_ps,protocol));
}
struct GeneralCompositeCostProvider::Impl {
    V document{nullptr};rtlzz::ArithmeticRequest request;std::unique_ptr<PublishedPrimitiveCostProvider> primitive;
    std::unique_ptr<GeneralStateModel> state;std::string primitive_sha,state_sha;
};
GeneralCompositeCostProvider::GeneralCompositeCostProvider(const Program& p,const rtlzz::ArithmeticRequest& r):CostProvider(costContext(r)),impl_(std::make_shared<Impl>()){
    impl_->request=r;impl_->document=load(r.model_bundle,r.model_bundle_sha256);const auto& b=*impl_->document.getAsObject();identity(b,r);
    if(text(b,"version")!=kGeneralComponentManifest||text(b,"quality_status")!="components_passed"||text(b,"evaluation_stage")!="grt")fail("general component manifest schema/quality/stage mismatch");
    flag(b,"joint_arithmetic_boundary_qualified",false);flag(b,"full_D_ready",false);flag(b,"shared_physical_area_qualified",false);
    const auto& components=object(b,"components");if(components.size()!=2)fail("general component manifest membership mismatch");
    auto path=[&](const O& ref){std::filesystem::path value=text(ref,"path");return (value.is_absolute()?value:std::filesystem::path(r.model_bundle).parent_path()/value).lexically_normal().string();};
    const auto& primitives=object(components,"primitives");const auto& state=object(components,"general_state");const auto& ancestor=object(b,"state_parent");
    if(text(primitives,"schema")!=kPublishedPrimitiveBundle||text(state,"schema")!=kGeneralStateModel||text(ancestor,"schema")!="arithmetic-native-state-qualified-v1")fail("general component referenced schema differs");
    impl_->primitive_sha=text(primitives,"sha256");impl_->state_sha=text(state,"sha256");
    auto child=r;child.model_bundle=path(primitives);child.model_bundle_sha256=impl_->primitive_sha;
    impl_->primitive=std::make_unique<PublishedPrimitiveCostProvider>(p,child);
    auto primitive=load(child.model_bundle,child.model_bundle_sha256);auto native=load(path(ancestor),text(ancestor,"sha256"));
    impl_->state=std::make_unique<GeneralStateModel>(path(state),impl_->state_sha,r);
    if(impl_->state->nativeParent()!=text(ancestor,"sha256")||text(*native.getAsObject(),"parent_primitives_sha256")!=impl_->primitive_sha||
        text(*primitive.getAsObject(),"measurement_static_identity_sha256")!=impl_->state->measurementIdentity())fail("general component parent/measurement identity mismatch");
}
std::string GeneralCompositeCostProvider::modelIdentity() const{return "asap7-general-components/"+impl_->request.model_bundle_sha256;}
CostEstimate GeneralCompositeCostProvider::node(const Program& p,NodeId id) const{return impl_->primitive->node(p,id);}
CostEstimate GeneralCompositeCostProvider::nodeInStage(const Program& p,NodeId id,const StageCostQuery& q) const{return impl_->primitive->nodeInStage(p,id,q);}
GeneralStateCostEstimate GeneralCompositeCostProvider::generalStateCost(const Program& p,const GeneralStateCostQuery& q) const{return impl_->state->price(p,q);}
StateCostEstimate GeneralCompositeCostProvider::stateCost(const Program&,const StateCostQuery&) const{fail("general_state_requires_explicit_service_and_unconditional_external_query");}
CostEstimate GeneralCompositeCostProvider::estimateGroup(const Program&,const std::vector<NodeId>&) const{fail("general_components_missing_qualified_whole_stage_and_interval_prices");}
double GeneralCompositeCostProvider::stateArea(std::uint64_t,std::size_t,std::size_t,std::size_t) const{fail("general_state_requires_full_typed_owner_query");}
void GeneralCompositeCostProvider::validateStateProtocol(bool reset,bool valid,bool) const{if(!reset||!valid)fail("general_state_requires_reset_and_valid");}
void GeneralCompositeCostProvider::validateInterface(const Program&,const std::string&) const{fail("general_components_missing_joint_G_and_shared_physical_qualification");}
double GeneralCompositeCostProvider::dataBudget(bool) const{fail("general_state_conditional_bounds_require_owner_query_and_joint_G");}
double GeneralCompositeCostProvider::stageDelay(double,std::size_t,std::uint64_t,const std::vector<NodeId>&) const{fail("general_components_missing_qualified_whole_stage");}
double GeneralCompositeCostProvider::stageArea(double,std::size_t,std::uint64_t,const std::vector<NodeId>&) const{fail("general_components_missing_qualified_whole_stage");}
std::string GeneralCompositeCostProvider::componentReport(const Program& p,const std::vector<int>& placement,int last,const std::vector<NodeId>& controls) const{
    auto report=llvm::json::parse(impl_->primitive->componentReport(p,placement,last,controls));if(!report){llvm::consumeError(report.takeError());fail("general primitive diagnostic JSON");}
    return llvm::formatv("{0:2}",V(O{{"version","general-component-coverage-v1"},{"model_id",modelIdentity()},
        {"general_state_sha256",impl_->state_sha},{"primitive_sha256",impl_->primitive_sha},{"general_state_query_required",true},
        {"general_state_checked_for_this_placement",false},{"pipeline_qualified",false},{"candidate_PPA_calls",0},
        {"primitive_report",std::move(*report)},
        {"missing_pipeline_components",llvm::json::Array{"joint_G_and_shared_physical","whole_stage","real_interval_when_split"}}})).str();
}
GeneralStateCostEstimate CostProvider::generalStateCost(const Program&,const GeneralStateCostQuery&) const{fail("provider has no qualified general state component");}
}
