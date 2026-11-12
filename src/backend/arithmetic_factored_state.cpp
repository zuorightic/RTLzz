// Reuses the frozen S projection, coefficients and protocol checks.
// Successor removes only the obsolete node/shared-pricing dependency.
#include "backend/arithmetic_factored_state.hpp"
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <functional>
#include <sstream>
#include <tuple>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;using F=std::map<std::string,double>;
[[noreturn]] void fail(const std::string& s){throw std::runtime_error(s);}
const O& object(const O& o,const char* k){auto x=o.getObject(k);if(!x)fail(std::string("missing state boundary object: ")+k);return *x;}
const A& array(const O& o,const char* k){auto x=o.getArray(k);if(!x)fail(std::string("missing state boundary array: ")+k);return *x;}
std::string text(const O& o,const char* k){auto x=o.getString(k);if(!x)fail(std::string("missing state boundary field: ")+k);return x->str();}
double number(const O& o,const char* k){auto x=o.getNumber(k);if(!x||!std::isfinite(*x)||*x<0)fail(std::string("invalid state boundary value: ")+k);return *x;}
std::string bytes(const std::string& path){auto b=llvm::MemoryBuffer::getFile(path);if(!b)fail("state boundary file unavailable: "+path);return (*b)->getBuffer().str();}
V load(const std::string& path,const std::string& hash){auto b=bytes(path);if(rtlzz::arithmeticHash(b)!=hash)fail("state boundary content hash mismatch");auto v=llvm::json::parse(b);if(!v){llvm::consumeError(v.takeError());fail("invalid state boundary JSON");}if(!v->getAsObject())fail("state boundary object required");return std::move(*v);}
bool bit(const std::vector<std::uint64_t>& v,int n){return std::size_t(n/64)<v.size()&&((v[n/64]>>(n%64))&1);}
unsigned ones(const std::vector<std::uint64_t>& v,const std::vector<std::uint64_t>& mask){unsigned n=0;for(std::size_t i=0;i<mask.size();++i){auto w=(i<v.size()?v[i]:0)&mask[i];while(w){w&=w-1;++n;}}return n;}
double predict(const O& m,const F& f){if(text(m,"kind")!="nonnegative_linear")fail("unsupported reused S predictor");const auto& names=array(m,"features");const auto& co=array(m,"coefficients");if(names.size()!=co.size())fail("reused S coefficient shape mismatch");double v=0;
    for(std::size_t i=0;i<names.size();++i){auto n=names[i].getAsString();auto c=co[i].getAsNumber();if(!n||!c||!std::isfinite(*c)||*c<0)fail("invalid reused S coefficient");v+=*c*f.at(n->str());}if(!std::isfinite(v))fail("reused S prediction overflow");return v;}
O json(const F& f){O o;for(const auto& [k,v]:f)o[k]=v;return o;}
F empty(){return {{"data_bits",0},{"data_signals",0},{"valid_bits",0},{"enable_domains",0},{"reset_one_bits",0}};}
struct Projection {std::map<std::string,F> owners;F control;double FF=0;std::vector<NodeId> controls;};
Projection project(const Program& p,const StateCostQuery& q,const O& domain){
    const auto& options=q.projection_options;
    auto rebuilt=stateCostQuery(p,options,q.placement,q.period_ps,q.external_outputs_included);
    if(!q.materialized_checked||!q.external_outputs_included||q.internal_stages!=rebuilt.internal_stages||q.valid_input_rtl_name!=rebuilt.valid_input_rtl_name||q.layout_sha256!=rebuilt.layout_sha256||q.graph_sha256!=rebuilt.graph_sha256||q.features!=rebuilt.features||q.input_variable_bits!=rebuilt.input_variable_bits||q.control_reset_one_bits!=rebuilt.control_reset_one_bits||q.slots.size()!=rebuilt.slots.size())fail("stale/unmaterialized state boundary query");
    for(std::size_t i=0;i<q.slots.size();++i){const auto& a=q.slots[i];const auto& b=rebuilt.slots[i];
        if(std::tie(a.origin,a.stage,a.enable_stage,a.declared_width,a.variable_bits,a.valid,a.external_capture,a.variable_mask,a.reset_values)!=std::tie(b.origin,b.stage,b.enable_stage,b.declared_width,b.variable_bits,b.valid,b.external_capture,b.variable_mask,b.reset_values))fail("forged state masks/reset/enable/lifetime");}
    if(q.internal_stages<1||q.internal_stages>number(domain,"max_internal_stages"))fail("native depth outside reused S candidate domain");
    Projection result;for(auto owner:{"input_service","native_state","external_state"})result.owners[owner]=empty();
    auto& input=result.owners["input_service"];NodeId iv=kInvalidNodeId,ov=kInvalidNodeId;std::set<int> enables;std::map<int,double> enabled_bits,enabled_words;std::set<int> valid_stages;
    for(const auto& port:p.ports) {
        if(port.name==options.input_valid_name){if(port.element_nodes.size()!=1)fail("invalid service valid port");iv=port.element_nodes.front();continue;}
        if(port.name==options.output_valid_name){if(port.element_nodes.size()!=1)fail("invalid output valid port");ov=port.element_nodes.front();}
        if(port.direction!=PortDirection::Input)continue;
        if(port.type.isArray()||port.element_nodes.size()!=1)fail("service state requires scalar public ports");
        const auto& s=p.signal(port.element_nodes.front());std::vector<std::uint64_t> mask((s.type.width+63)/64);
        for(int b=0;b<s.type.width;++b){if(s.value.valid&&bit(s.value.known_one,b))fail("known-one data outside reused S domain");if(!(s.value.valid&&(s.value.constant||bit(s.value.known_zero,b))))mask[b/64]|=std::uint64_t(1)<<(b%64);}
        auto count=ones(mask,mask);if(count>number(domain,"max_variable_word_bits"))fail("service word outside reused S candidate domain");
        if(count){input["data_bits"]+=count;++input["data_signals"];auto reset=options.input_reset_values.find(port.name);if(reset!=options.input_reset_values.end())input["reset_one_bits"]+=ones(reset->second,mask);}
    }
    if(iv==kInvalidNodeId||ov==kInvalidNodeId)fail("actual valid input/output missing");
    auto reset=options.input_reset_values.find(options.input_valid_name);if(reset!=options.input_reset_values.end()&&std::any_of(reset->second.begin(),reset->second.end(),[](auto v){return v!=0;}))fail("nonzero service-valid reset unsupported");
    result.controls={iv,ov};input["valid_bits"]=1;input["enable_domains"]=input["data_signals"]+1;
    if(input["data_bits"]+1!=q.input_variable_bits)fail("service/core input bit inventory differs");
    for(const auto& slot:q.slots){auto& f=result.owners[slot.external_capture?"external_state":"native_state"];
        if(slot.valid){if(slot.variable_bits!=1||slot.declared_width!=1||slot.enable_stage!=-1||slot.reset_values!=std::vector<std::uint64_t>{0})fail("unsupported valid/reset protocol");++f["valid_bits"];valid_stages.insert(slot.stage);continue;}
        if(slot.enable_stage!=slot.stage-1||slot.variable_bits>number(domain,"max_variable_word_bits"))fail("unsupported state enable or word size");
        const auto& s=p.signal(slot.origin);for(auto v:s.value.known_one)if(v)fail("known-one state outside reused S domain");
        if(slot.variable_bits){f["data_bits"]+=slot.variable_bits;++f["data_signals"];f["reset_one_bits"]+=ones(slot.reset_values,slot.variable_mask);
            if(!slot.external_capture){enables.insert(slot.enable_stage);enabled_bits[slot.stage]+=slot.variable_bits;++enabled_words[slot.stage];}}
    }
    if(valid_stages.size()!=std::size_t(q.internal_stages+1)||result.owners["native_state"]["valid_bits"]!=q.internal_stages||result.owners["external_state"]["valid_bits"]!=1)fail("valid-chain/external capture inventory incomplete");
    result.owners["native_state"]["enable_domains"]=enables.size();
    if(input["data_bits"]>number(domain,"max_input_data_bits")||result.owners["native_state"]["data_bits"]>number(domain,"max_native_data_bits")||result.owners["external_state"]["data_bits"]>number(domain,"max_external_data_bits"))fail("state bank outside reused S candidate size");
    double largest=0,words=0;for(const auto& [stage,bits]:enabled_bits)largest=std::max(largest,bits);for(const auto& [stage,n]:enabled_words)words=std::max(words,n);
    if(words>number(domain,"max_parallel_words"))fail("parallel state bank outside reused S candidate size");
    result.control={{"one",1},{"native_enable_bits_max",largest},{"native_enable_bits_sqrt",std::sqrt(largest)},
        {"native_enable_bits_log2",std::log2(1+largest)},{"native_enable_words_max",words},{"native_valid_targets_max",largest+2},
        {"native_data_bits",result.owners["native_state"]["data_bits"]},{"native_reset_one_bits",result.owners["native_state"]["reset_one_bits"]},
        {"input_data_bits",input["data_bits"]},{"external_data_bits",result.owners["external_state"]["data_bits"]}};
    for(const auto& [owner,f]:result.owners)result.FF+=f.at("data_bits")+f.at("valid_bits");return result;
}
}
struct FactoredStateBoundaryProvider::Impl {
    V binding{nullptr},state{nullptr};rtlzz::ArithmeticRequest request;std::string sha,state_sha,service_sha;StateBoundaryUse use;
};
FactoredStateBoundaryProvider::FactoredStateBoundaryProvider(const std::string& path,const std::string& identity,const rtlzz::ArithmeticRequest& request,StateBoundaryUse use):impl_(std::make_shared<Impl>()) {
    impl_->binding=load(path,identity);impl_->sha=identity;impl_->request=request;impl_->use=use;const auto& b=*impl_->binding.getAsObject();
    if(text(b,"version")!="conditional-boundary-binding-v1"||text(b,"quality_status")!="candidate_reuse"||text(b,"target_platform")!=request.target_platform||text(b,"profile_sha256")!=request.platform_profile_sha256||text(b,"operator_boundary_policy")!=request.operator_boundary_policy||b.getBoolean("full_D_ready")!=false)fail("state boundary flow/schema/scope mismatch");
    auto resolve=[&](const O& entry){auto ref=std::filesystem::path(text(entry,"path"));return (ref.is_absolute()?ref:std::filesystem::path(path).parent_path()/ref).lexically_normal().string();};
    const auto& s=object(b,"state_model");impl_->state_sha=text(s,"sha256");impl_->state=load(resolve(s),impl_->state_sha);const auto& model=*impl_->state.getAsObject();
    if(text(model,"version")!="arithmetic-general-state-qualified-v1"||text(model,"quality_status")!="passed"||text(model,"platform_profile_sha256")!=request.platform_profile_sha256||model.getBoolean("shared_clock_physical_repair_prices_included")!=false)fail("reused S identity/scope mismatch");
    const auto& original_boundary=object(object(object(model,"stages"),"grt"),"boundary_model");
    if(V(O(object(b,"control_models")))!=V(O(object(original_boundary,"control_models")))||V(A(array(b,"control_exact_clocks")))!=V(A(array(model,"exact_SDC_ps"))))fail("reused S control coefficients or clock identity changed");
    const auto& six=object(b,"six_path_upper_ps");if(six.size()!=6)fail("reused S six-path inventory changed");
    for(auto name:{"input_data","input_enable","input_valid","native_data","external_data","external_valid"}) {
        double maximum=0;for(const auto& clock:object(original_boundary,"immutable_by_exact_SDC")){auto paths=clock.second.getAsObject();if(!paths)fail("invalid original S bounds");maximum=std::max(maximum,number(object(*paths,name),"overhead_ps"));}
        if(number(six,name)!=maximum)fail("reused S path bound changed");
    }
    const auto& service=object(b,"service_contract");auto raw=bytes(resolve(service));impl_->service_sha=text(service,"sha256");if(rtlzz::arithmeticHash(raw)!=impl_->service_sha)fail("public service witness changed");
    std::set<std::string> expected_tokens{"assign wen_iv = push__vld;","if (wen_iv__)","__vul_pipeline_q_iv <= wdata_iv__;","__vul_pipeline_q_ov <= __vul_pipeline_d_ov;"},tokens;
    for(const auto& v:array(service,"required_tokens")){auto token=v.getAsString();if(!token||raw.find(token->str())==std::string::npos||!tokens.insert(token->str()).second)fail("public service protocol witness mismatch");}
    if(tokens!=expected_tokens)fail("public service proof inventory incomplete");
    if(text(service,"data_enable_policy")!="independent_per_input_port"||text(service,"valid_enable_policy")!="independent_held_valid"||text(service,"external_capture")!="unconditional")fail("unsupported public service protocol");
}
ConditionalBoundaryPrice FactoredStateBoundaryProvider::price(const Program& p,const StateCostQuery& q,const std::vector<ConditionalBoundaryRequest>& requests,const std::string& parent) const {
    ConditionalBoundaryPrice result;result.model_sha256=impl_->sha;result.state_model_sha256=impl_->state_sha;result.service_contract_sha256=impl_->service_sha;result.operator_model_sha256=parent;result.layout_sha256=q.layout_sha256;result.target_period_ps=q.period_ps;
    try {
        const auto& b=*impl_->binding.getAsObject();const auto& r=impl_->request;const auto& domain=object(b,"candidate_domain");
        if(parent!=r.model_bundle_sha256||q.period_ps!=r.target_period_ps||q.period_ps<number(domain,"target_min_ps")||q.period_ps>number(domain,"target_max_ps"))fail("state boundary target/node-model dependency mismatch or outside candidate interval");
        const auto projection=project(p,q,domain);auto actual=conditionalBoundaryRequests(p,q.placement,q.internal_stages,projection.controls);
        if(actual.size()!=requests.size())fail("state boundary stage inventory mismatch");
        for(std::size_t i=0;i<actual.size();++i){const auto& a=actual[i];const auto& x=requests[i];if(a.cut.index!=x.cut.index||a.cut.last!=x.cut.last||a.cut.graph_sha256!=x.cut.graph_sha256||a.cut.placement!=x.cut.placement||a.cut.controls!=x.cut.controls||a.cut.cut_sha256!=x.cut.cut_sha256||a.cut.members!=x.cut.members||a.launches!=x.launches||a.captures!=x.captures||a.edges!=x.edges||a.cut.captures.size()!=x.cut.captures.size())fail("stale state boundary endpoints/wires");for(std::size_t j=0;j<a.cut.captures.size();++j)if(a.cut.captures[j].value!=x.cut.captures[j].value||a.cut.captures[j].banks!=x.cut.captures[j].banks)fail("forged state boundary capture bank");}
        const auto& areas=object(object(object(*impl_->state.getAsObject(),"stages"),"grt"),"area_models");O features;
        double functional=0;
        for(const auto& [owner,f]:projection.owners){auto area=predict(object(areas,owner.c_str()),f);result.owner_area_um2[owner]=area;functional+=area;features[owner]=json(f);}
        // The successor owns S only. Arithmetic and shared physical prices
        // are supplied once by the outer node/region composition provider.
        const auto& shared=object(b,"shared");
        const auto& six=object(b,"six_path_upper_ps");const auto& controls=object(b,"control_models");
        auto upper=[&](const char* name){const auto& model=object(controls,name);double bound=0;for(const auto& v:array(b,"control_exact_clocks")){auto t=v.getAsInteger();if(!t)fail("invalid reused S clock inventory");auto f=projection.control;for(const auto& clock:array(b,"control_exact_clocks")){auto n=clock.getAsInteger();f["exact_SDC_"+std::to_string(*n)]=*n==*t;}
                bound=std::max(bound,predict(object(model,"regression"),f)+number(model,"train_upper_correction_ps")+number(model,"reserve_ps"));}return bound;};
        const double enable_upper=upper("native_enable"),valid_upper=upper("native_valid");A checks;
        for(const auto& request:requests){ConditionalStageBoundary stage;stage.cut_sha256=request.cut.cut_sha256;stage.other_timing_violation_ps=0.;
            // S publishes complete boundary sums, not separate clk->Q/setup
            // regressions. Use a zero-relative launch reference and deduct the
            // whole frozen sum at capture exactly once.
            for(auto id:request.launches)stage.launch_arrival_ps[id]=0.;
            for(auto id:request.captures)stage.capture_required_ps[id]=q.period_ps-number(six,request.cut.index==q.internal_stages?"external_data":"native_data");
            for(auto edge:request.edges)stage.wire_delay_ps[edge]=number(shared,"wire_per_edge_ps");
            result.stages.push_back(std::move(stage));}
        auto check=[&](const char* name,int stage,double needed,double budget){auto violation=std::max(0.,needed-budget);result.stages.at(stage).other_timing_violation_ps=std::max(result.stages.at(stage).other_timing_violation_ps,violation);checks.push_back(O{{"path_class",name},{"stage",stage},{"upper_ps",needed},{"budget_ps",budget},{"violation_ps",violation}});};
        for(auto name:{"input_data","input_enable","input_valid"})check(name,0,number(six,name),.8*q.period_ps);
        for(int stage=0;stage<q.internal_stages;++stage){bool data=false;for(const auto& slot:q.slots)if(!slot.valid&&!slot.external_capture&&slot.stage==stage+1&&slot.variable_bits)data=true;
            if(data)check("native_enable",stage,enable_upper,q.period_ps);check("native_valid",stage,valid_upper,q.period_ps);}
        check("external_valid",q.internal_stages,number(six,"external_valid"),q.period_ps);
        check("output_data",q.internal_stages,number(six,"external_data"),.8*q.period_ps);check("output_valid",q.internal_stages,number(six,"external_valid"),.8*q.period_ps);
        // State that merely forwards a value can be absent from arithmetic
        // cuts. Inspect every actual data slot rather than assuming its path
        // is represented by a priced arithmetic capture.
        std::function<bool(NodeId,int,std::set<NodeId>&)> arithmetic_here=[&](NodeId id,int stage,std::set<NodeId>& seen){if(!seen.insert(id).second)return false;const auto& s=p.signal(id);if(q.placement[id]!=stage||s.value.constant)return false;const auto& d=*s.driver;
            if(d.kind==OperationKind::AddCarry||(d.kind==OperationKind::Binary&&(d.op==OpCode::Mul||d.op==OpCode::Add)))return true;
            for(const auto& a:d.operands)if(a.kind==OperandKind::Symbol&&arithmetic_here(a.node,stage,seen))return true;return false;};
        for(const auto& slot:q.slots)if(!slot.valid&&slot.variable_bits){std::set<NodeId> seen;int stage=slot.stage-1;if(!arithmetic_here(slot.origin,stage,seen))check(slot.external_capture?"external_bypass":"native_forward_or_bypass",stage,number(six,slot.external_capture?"external_data":"native_data"),q.period_ps);}
        O diagnostics{{"version","actual-factored-state-boundary-v1"},{"state_model_sha256",impl_->state_sha},{"binding_sha256",impl_->sha},{"service_contract_sha256",impl_->service_sha},
            {"owner_features",std::move(features)},{"control_features",json(projection.control)},{"total_FF_bits",projection.FF},{"nonarithmetic_checks",std::move(checks)},
            {"launch_reference","zero-relative; complete S boundary sum assigned once to capture"},{"input_IO_scope","port-to-input-service only; not charged again to arithmetic launches"},
            {"S_coefficients_changed",false},{"candidate_reuse_only",true},{"required_validation",A(array(b,"required_validation"))},{"full_D_ready",false}};
        result.diagnostics_json=llvm::formatv("{0:2}",V(std::move(diagnostics))).str();result.joint_qualified=false;
        if(impl_->use==StateBoundaryUse::RequireQualified)fail("reused S needs explicit new ABC/SDC/service/shared/arithmetic joint validation; candidate diagnostics available");
        result.covered=true;
    }catch(const std::exception& e){result.reason=std::string(e.what());}
    return result;
}
}
