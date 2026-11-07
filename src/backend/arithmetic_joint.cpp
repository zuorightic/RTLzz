#include "backend/arithmetic_joint.hpp"
#include "backend/arithmetic_regions.hpp"
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
using O=llvm::json::Object;using V=llvm::json::Value;using A=llvm::json::Array;
[[noreturn]] void fail(const std::string& reason){throw rtlzz::ArithmeticError({"joint_component_cost","cost_uncovered",reason,{},{}});}
std::string read(const std::string& path){std::ifstream f(path,std::ios::binary);if(!f)fail("joint component unavailable: "+path);return {std::istreambuf_iterator<char>(f),{}};}
V load(const std::string& path,const std::string& hash){auto bytes=read(path);if(rtlzz::arithmeticHash(bytes)!=hash)fail("joint component content hash mismatch");
    auto v=llvm::json::parse(bytes);if(!v){llvm::consumeError(v.takeError());fail("invalid joint component JSON");}if(!v->getAsObject())fail("joint object required");return std::move(*v);}
const O& object(const O& o,const char* key){auto x=o.getObject(key);if(!x)fail(std::string("joint object missing: ")+key);return *x;}
const A& array(const O& o,const char* key){auto x=o.getArray(key);if(!x)fail(std::string("joint array missing: ")+key);return *x;}
std::string text(const O& o,const char* key){auto x=o.getString(key);if(!x)fail(std::string("joint string missing: ")+key);return x->str();}
double number(const O& o,const char* key){auto x=o.getNumber(key);if(!x||!std::isfinite(*x))fail(std::string("invalid joint number: ")+key);return *x;}
void flag(const O& o,const char* key,bool expected){auto x=o.getBoolean(key);if(!x||*x!=expected)fail(std::string("joint qualification flag mismatch: ")+key);}
bool bit(const std::vector<std::uint64_t>& a,unsigned b){return b/64<a.size()&&(a[b/64]>>(b%64)&1);}
NodeId alias(const Program& p,NodeId id){std::set<NodeId> seen;while(seen.insert(id).second){const auto& s=p.signal(id);if(!s.driver)break;const auto& d=*s.driver;
    if((d.kind!=OperationKind::Assign&&d.kind!=OperationKind::Cast)||d.operands.size()!=1||d.operands[0].kind!=OperandKind::Symbol||d.operands[0].signed_view||d.signed_truncation||!sameType(s.type,d.operands[0].type))break;id=d.operands[0].node;}return id;}
bool wiring(const Signal& s){return s.value.valid&&s.value.constant||s.driver&&(s.driver->kind==OperationKind::Assign||s.driver->kind==OperationKind::Cast||
    s.driver->kind==OperationKind::PortRead||s.driver->kind==OperationKind::ZExt||s.driver->kind==OperationKind::Trunc||s.driver->kind==OperationKind::Slice||s.driver->kind==OperationKind::Concat);}
double prediction(const O& m,const std::map<std::string,double>& features){
    const auto& names=array(m,"features");const auto& coefficients=array(m,"coefficients");if(names.size()!=coefficients.size()||names.empty())fail("joint predictor dimensions");
    double value=0;std::set<std::string> seen;for(std::size_t i=0;i<names.size();++i){auto name=names[i].getAsString();auto c=coefficients[i].getAsNumber();
        if(!name||!c||!std::isfinite(*c)||*c<0||!seen.insert(name->str()).second)fail("joint predictor feature/coefficient");
        auto at=features.find(name->str());if(at==features.end()||!std::isfinite(at->second)||at->second<0)fail("joint predictor feature unavailable");value+=*c*at->second;}
    auto scale=number(m,"validation_scale");if(scale<1||!std::isfinite(value*scale)||value*scale<0)fail("invalid frozen joint calibration");return value*scale;
}
CompletePlacementCostQuery checked(const Program& p,const CompletePlacementCostQuery& supplied){
    const auto& c=supplied.state.core;auto q=completePlacementCostQuery(p,c.projection_options,c.placement,c.period_ps,supplied.state.protocol);
    auto normalized=supplied;normalized.state=q.state;
    if(supplied.state.layout_sha256!=q.state.layout_sha256||generalStateSemanticFingerprint(supplied.state)!=q.state.layout_sha256||
       supplied.state.core.features!=q.state.core.features||supplied.state.core.slots.size()!=q.state.core.slots.size()||
       supplied.query_sha256!=q.query_sha256||completePlacementCostQueryJson(normalized)!=completePlacementCostQueryJson(q))
        fail("stale or forged joint placement/context");return q;
}
// Filled from the fixed independent synthetic reference program registration,
// then checked against public-source RTL semantics in the binding regression.
// No model metadata or caller-supplied domain name can alter this allowlist.
const std::map<std::string,std::string> shapes{
    {"mul32-shared","00dcdad4770330d4bc880e2b1316273e268003fb4e8e804a4288bc084b170969"},
    {"mul16-shared","03bb6d1e55d28199240d93527e30035338e26881496994370010bb2afa716cb4"},
    {"safe-div16","19e95a3ee31570f161f4be1f31dcdb5a99d8c58819cd32eb9744f7870d642964"},
    {"safe-div32-select","04528d67ac25fd070ea3d8df8e7f77ad7d46b2a287f99b76cba14162c8ef1a1d"},
    {"atomicfp-two","50e66b0ec89444addbb7fe5c62b5724f782afa6a55379ad8c8e3965b93c12ec2"},
    {"atomicfp-side","2b81e374dd62a1e7826dc5072e8e998b962297a8ea2dd318cfec7a4c009d9ca7"},
    {"addmux","0da04281c42c4f386719b48e6d036e7a657da8f1c2a01e54739309b42e9baeef"},
    {"safe-div32-three","3a0005deef360970ac2674effb06a999f65423e6d1e72f8f5073d9101e6ad19d"},
    {"atomicfp-select","0d7de626c308ebd546a31b95df0b18e7a7bd2d5e3ad2acaff0a5792af6f36dcb"},
};
struct Component {
    V model{nullptr},contracts{nullptr},capabilities{nullptr};std::string hash,contracts_hash;
    const O& bundle() const{return *model.getAsObject();}
    const O& domains() const{return object(object(object(bundle(),"stages"),"grt"),"domains");}
    const O& contractDomains() const{return object(*contracts.getAsObject(),"domains");}
};
}
struct JointCompositeCostProvider::Impl {
    V manifest{nullptr};Component G,I;rtlzz::ArithmeticRequest request;const Program* program=nullptr;
    mutable std::map<std::string,std::string> core_domains;
};
JointCompositeCostProvider::JointCompositeCostProvider(const Program& p,const rtlzz::ArithmeticRequest& r):CostProvider(costContext(r)),impl_(std::make_shared<Impl>()){
    impl_->request=r;impl_->program=&p;impl_->manifest=load(r.model_bundle,r.model_bundle_sha256);const auto& m=*impl_->manifest.getAsObject();
    if(text(m,"version")!=kJointComponentManifest||text(m,"quality_status")!="components_passed"||text(m,"target_platform")!="asap7"||
       r.target_platform!="asap7"||text(m,"platform_profile_sha256")!=r.platform_profile_sha256||rtlzz::arithmeticHash(read(r.platform_profile))!=r.platform_profile_sha256||
       text(m,"evaluation_stage")!="grt"||number(m,"ABC_ps")!=1500||text(m,"IO")!="0.2T")fail("joint manifest profile/platform/stage/ABC/IO mismatch");
    flag(m,"full_D_ready",false);const auto& components=object(m,"components");if(components.size()!=2)fail("G/I component membership mismatch");
    auto path=[&](const O& ref){std::filesystem::path x=text(ref,"path");return (x.is_absolute()?x:std::filesystem::path(r.model_bundle).parent_path()/x).lexically_normal().string();};
    auto loadComponent=[&](Component& c,const char* family,int count,const char* version,const char* contracts_version){
        const auto& ref=object(components,family);const auto& model=object(ref,"model");const auto& contracts=object(ref,"contracts");const auto& caps=object(ref,"capabilities");
        c.hash=text(model,"sha256");c.contracts_hash=text(contracts,"sha256");c.model=load(path(model),c.hash);c.contracts=load(path(contracts),c.contracts_hash);c.capabilities=load(path(caps),text(caps,"sha256"));
        const auto& b=c.bundle();const auto& d=*c.contracts.getAsObject();const auto& a=*c.capabilities.getAsObject();
        if(text(b,"version")!=version||text(b,"quality_status")!="passed"||text(b,"family")!=family||text(b,"target_platform")!="asap7"||
           text(b,"platform_profile_sha256")!=r.platform_profile_sha256||number(b,"ABC_ps")!=1500||text(b,"IO")!="0.2T"||
           object(b,"stages").size()!=1||text(object(object(b,"stages"),"grt"),"status")!="passed"||
           text(d,"version")!=contracts_version||text(d,"model_sha256")!=c.hash||number(d,"qualified_domain_count")!=count||
           c.domains().size()!=std::size_t(count)||c.contractDomains().size()!=std::size_t(count)||text(a,"model_sha256")!=c.hash||
           number(a,"qualified_domain_count")!=count||text(a,"qualified_stage")!="grt")fail("unqualified G/I schema/parameters/contracts/stage");
        flag(b,"full_D_ready",false);flag(a,"full_D_ready",false);
        for(const auto& entry:c.domains()){
            const auto& parameters=*entry.second.getAsObject();if(!c.contractDomains().getObject(entry.first))fail("G/I domain membership differs");
            const auto& models=object(parameters,"models");if(models.size()!=std::size_t(std::string(family)=="G"?6:2))fail("G/I quantity ownership differs");
            std::map<std::string,double> zero{{"one",1},{"input_reset_one_bits",0},{"capture_reset_one_bits",0}};
            for(const auto& quantity:models)prediction(*quantity.second.getAsObject(),zero);
            if(std::string(family)=="G"){
                const auto& bounds=object(parameters,"joint_boundary");if(number(bounds,"reserve_ps")!=50||number(bounds,"data_capture_clock_upper_ps")<50)fail("G data boundary reserve");
                const auto& control=object(bounds,"control");if(control.size()!=7)fail("G complete control membership");
                for(const auto* name:{"native_enable","valid","external_data","external_valid","input_data","input_enable","input_valid"}){
                    const auto& bound=object(control,name);if(number(bound,"upper_ps")<50||text(bound,"kind")!=(std::string(name).rfind("input_",0)==0?"port":"register"))fail("G control boundary kind/reserve");}
            }
        }
    };
    loadComponent(impl_->G,"G",9,"asap7-exact-G-single-valid-component-v1","asap7-exact-G-query-contracts-v1");
    loadComponent(impl_->I,"I",42,"asap7-exact-I-component-v1","asap7-exact-I-query-contracts-v1");
    if(text(impl_->G.bundle(),"measurement_static_identity_sha256")!=text(impl_->I.bundle(),"measurement_static_identity_sha256"))fail("G/I physical measurement identities differ");
}
void JointCompositeCostProvider::validateStateProtocol(bool reset,bool valid,bool) const{if(!reset||!valid)fail("joint G/I requires reset and valid protocol");}
void JointCompositeCostProvider::validateInterface(const Program& p,const std::string& valid_output) const{
    if(valid_output.empty())fail("joint G/I requires explicit valid output");validate(p,"joint_component_preflight");regions::validate(p);
    if(impl_->request.latency!=2)fail("G uncovered_native_depth: published shell has one native level; actual L="+std::to_string(impl_->request.latency));
    // PipelineV2 owns the valid chain; the source output may still be a
    // constant placeholder here. This preflight only rejects unknown data
    // topologies. The complete query checks the configured valid ports and
    // the actual native/external chain, so a guessed input cannot grant cost.
    bool registered=false;for(const auto& in:p.ports)if(in.direction==PortDirection::Input&&in.type.width==1){
        try{auto fingerprint=jointCoreFingerprint(p,in.name,valid_output);
            for(const auto& [name,value]:shapes)if(value==fingerprint)registered=true;
        }catch(const rtlzz::ArithmeticError& e){if(e.diagnostic.failure_code!="cost_uncovered")throw;}
    }
    if(!registered)fail("G uncovered_stage_topology: actual typed core DAG is outside nine registered synthetic cores");
}
std::string JointCompositeCostProvider::modelIdentity() const{return "asap7-joint-components/"+impl_->request.model_bundle_sha256;}
CostEstimate JointCompositeCostProvider::node(const Program&,NodeId) const{fail("joint G/I node price deferred to complete typed placement");}
CostEstimate JointCompositeCostProvider::estimateGroup(const Program&,const std::vector<NodeId>&) const{fail("joint G/I requires complete typed placement");}
std::vector<ExactIntervalPrice> JointCompositeCostProvider::intervalPrices(const Program& p,const CompletePlacementCostQuery& supplied) const{
    auto q=checked(p,supplied);validate(p,"joint_interval_component");regions::validate(p);std::vector<ExactIntervalPrice> prices;
    pipeline::Program reset;reset.combinational=p;reset.rtl_program=p;reset.options=q.state.core.projection_options;
    std::set<NodeId> nodes;for(const auto& i:q.intervals){nodes.insert(i.inputs.begin(),i.inputs.end());nodes.insert(i.outputs.begin(),i.outputs.end());}
    for(auto id:nodes)reset.registers.push_back({id,id,0});pipeline::initializeResetValues(reset);std::map<NodeId,std::vector<std::uint64_t>> values;
    for(const auto& r:reset.registers)values[r.node]=r.reset_values;
    for(const auto& i:q.intervals){
        int width=i.parameters.at("lhs_width"),block=i.parameters.at("block_width"),result=i.parameters.at("result_width");
        if(width!=i.parameters.at("rhs_width")||(width!=16&&width!=32)||(block!=width/2&&block!=width/4)||(result!=width&&result!=2*width))fail("I native width/shape unsupported");
        std::string domain="w"+std::to_string(width)+"-"+(result==width?"low":"full")+"-"+(block==width/2?"half":"quarter")+"-g"+std::to_string(i.first)+"-"+std::to_string(i.last);
        auto contract=impl_->I.contractDomains().getObject(domain);auto parameters=impl_->I.domains().getObject(domain);
        if(!contract||!parameters||number(*contract,"period_ps")!=q.state.core.period_ps||q.state.core.period_ps!=impl_->request.target_period_ps)fail("I exact domain/target unsupported: "+domain);
        std::map<std::string,double> f{{"one",1},{"width",double(width)},{"block_width",double(block)},{"first",double(i.first)},{"last",double(i.last)},
            {"input_bits",0},{"capture_bits",0},{"input_signals",double(i.inputs.size())},{"input_reset_one_bits",0},{"capture_reset_one_bits",0}};
        std::vector<int> widths;for(auto id:i.inputs){const auto& bits=i.input_bit_positions.at(id);widths.push_back(bits.size());f["input_bits"]+=bits.size();for(auto b:bits)f["input_reset_one_bits"]+=bit(values.at(id),b);}
        for(auto id:i.outputs){const auto& bits=i.output_bit_positions.at(id);f["capture_bits"]+=bits.size();for(auto b:bits)f["capture_reset_one_bits"]+=bit(values.at(id),b);}
        auto expected=object(*contract,"static_features");for(const auto& entry:expected)if(f.at(entry.first.str())!=*entry.second.getAsNumber())fail("I actual trimmed bit geometry differs: "+domain);
        std::vector<int> registered;for(const auto& v:array(*contract,"input_widths"))registered.push_back(*v.getAsInteger());
        // Native input IDs are structural ordering, not source symbol names.
        if(widths!=registered)fail("I actual input word packing differs");
        const auto& models=object(*parameters,"models");ExactIntervalPrice price;price.domain_id=domain;price.query_sha256=i.query_sha256;price.model_sha256=impl_->I.hash;
        price.topology_sha256=text(*contract,"interval_topology_sha256");price.features=f;price.features.erase("one");
        price.QD_ps=prediction(object(models,"arithmetic_QD"),f);price.arithmetic_area_um2=prediction(object(models,"arithmetic_area"),f);prices.push_back(std::move(price));
    }return prices;
}
CompletePlacementPrice JointCompositeCostProvider::completePlacementCost(const Program& p,const CompletePlacementCostQuery& supplied) const{
    auto q=checked(p,supplied);const auto& state=q.state;const auto& options=state.core.projection_options;
    if(state.core.period_ps!=impl_->request.target_period_ps)fail("G query/request clock mismatch");
    if(state.core.internal_stages!=1||q.stages.size()!=2)fail("G uncovered_native_depth: published shell has exactly one native level");
    if(!q.intervals.empty())fail("G uncovered_I_mixed_stage: I arithmetic does not grant joint state/shared/boundary qualification");
    if(state.protocol.mode!=1||!state.protocol.external_data_unconditional||!state.protocol.external_valid_unconditional||
       !options.reset_enabled||!options.reset_active_low||state.core.control_reset_one_bits)fail("G service/reset/external protocol differs");
    for(auto id:q.stages[1].members)if(!wiring(p.signal(id)))fail("G uncovered_stage_topology: arithmetic after native bank");
    auto graph=rtlzz::arithmeticHash(emitText(p))+"/"+options.input_valid_name+"/"+options.output_valid_name;std::string domain;
    if(auto at=impl_->core_domains.find(graph);at!=impl_->core_domains.end())domain=at->second;
    else{auto fingerprint=jointCoreFingerprint(p,options.input_valid_name,options.output_valid_name);
        for(const auto& [name,expected]:shapes)if(expected==fingerprint){domain=name;break;}
        impl_->core_domains[graph]=domain;}
    if(domain.empty())fail("G uncovered_stage_topology: actual typed core DAG is outside nine registered synthetic cores");
    const auto& contract=object(impl_->G.contractDomains(),domain.c_str());if(number(contract,"period_ps")!=state.core.period_ps)fail("G exact target differs for actual core: "+domain);
    std::map<std::string,double> f{{"one",1},{"input_bits",0},{"capture_bits",0},{"input_signals",0},{"capture_signals",0},{"input_reset_one_bits",0},{"capture_reset_one_bits",0}};
    std::map<std::string,unsigned> valid_counts;std::map<NodeId,const StateSlot*> natives,externals;
    for(const auto& item:state.slots){const auto& s=item.state;
        if(s.valid){for(auto w:s.reset_values)if(w)fail("G valid reset must remain zero");++valid_counts[item.owner];continue;}
        if(item.owner=="input_service"){
            if(s.stage!=0||item.write_enable!=state.protocol.valid_write_enable)fail("G service shared enable differs");
            f["input_bits"]+=s.variable_bits;f["input_signals"]+=1;for(unsigned b=0;b<unsigned(s.declared_width);++b)if(bit(s.variable_mask,b))f["input_reset_one_bits"]+=bit(s.reset_values,b);
        }else if(item.owner=="native_state"){
            if(s.stage!=1||s.enable_stage!=0||!natives.emplace(alias(p,s.origin),&s).second)fail("G native capture topology/duplicate owner differs");
            f["capture_bits"]+=s.variable_bits;f["capture_signals"]+=1;for(unsigned b=0;b<unsigned(s.declared_width);++b)if(bit(s.variable_mask,b))f["capture_reset_one_bits"]+=bit(s.reset_values,b);
        }else if(item.owner=="external_state"){
            if(s.stage!=2||s.enable_stage!=-1||!externals.emplace(alias(p,s.origin),&s).second)fail("G unconditional external capture topology differs");
        }else fail("G foreign state owner");
    }
    for(const auto* owner:{"input_service","native_state","external_state"})if(valid_counts[owner]!=1)fail("G valid chain geometry differs");
    if(natives.size()!=externals.size())fail("G complete native/external bank membership differs");
    for(const auto& [id,s]:natives){auto at=externals.find(id);if(at==externals.end()||s->declared_width!=at->second->declared_width||s->variable_mask!=at->second->variable_mask||s->reset_values!=at->second->reset_values)fail("G data lifetime/declared mask/reset differs");}
    for(const auto& entry:object(contract,"static_features"))if(f.at(entry.first.str())!=*entry.second.getAsNumber())fail("G actual service/capture geometry differs: "+domain);
    const auto& parameters=object(impl_->G.domains(),domain.c_str());const auto& models=object(parameters,"models");const auto& bounds=object(parameters,"joint_boundary");
    CompletePlacementPrice result;result.query_sha256=q.query_sha256;result.model_sha256=context_.model_bundle_sha256;result.joint_source_and_boundary_qualified=true;
    for(const auto& stage:q.stages){StagePlacementPrice price;price.index=stage.index;price.cut_sha256=stage.cut_sha256;price.state_boundary_sha256=q.storage_boundaries[stage.index].sha256;
        price.model_sha256=impl_->G.hash;price.ownership="whole_stage";price.qualified_domain=domain;price.owned_members=stage.members;
        if(stage.index==0){price.QD_ps=prediction(object(models,"arithmetic_QD"),f);price.combinational_area_um2=prediction(object(models,"arithmetic_area"),f);price.clock_capture_overhead_ps=number(bounds,"data_capture_clock_upper_ps");}
        else price.clock_capture_overhead_ps=number(object(object(bounds,"control"),"external_data"),"upper_ps");
        result.stages.push_back(std::move(price));}
    for(const auto* owner:{"input_service","native_state","external_state"}){result.functional_state_area_um2[owner]=prediction(object(models,(std::string(owner)+"_area").c_str()),f);result.state_owner_model_sha256[owner]=impl_->G.hash;}
    result.shared_physical_area_um2=prediction(object(models,"shared_physical_and_clock_area"),f);result.shared_physical_model_sha256=impl_->G.hash;
    for(const auto& entry:object(bounds,"control"))result.control_boundary_upper_ps[entry.first=="valid"?"native_valid":entry.first.str()]=number(*entry.second.getAsObject(),"upper_ps");
    return result;
}
std::string JointCompositeCostProvider::componentReport(const Program&,const std::vector<int>&,int,const std::vector<NodeId>&) const{
    return llvm::formatv("{0:2}",V(O{{"version","joint-component-consumer-v1"},{"G_sha256",impl_->G.hash},{"I_sha256",impl_->I.hash},
        {"requires_actual_complete_placement",true},{"all_candidate_domains_qualified",false},{"PPA_calls",0},{"full_D_ready",false}})).str();
}
std::string exactIntervalPricesJson(const std::vector<ExactIntervalPrice>& prices){A rows;for(const auto& p:prices){O features;for(const auto& [name,value]:p.features)features[name]=value;
    rows.push_back(O{{"domain_id",p.domain_id},{"query_sha256",p.query_sha256},{"model_sha256",p.model_sha256},{"topology_sha256",p.topology_sha256},
        {"arithmetic_QD",p.QD_ps},{"arithmetic_area",p.arithmetic_area_um2},{"features",std::move(features)}});}return llvm::formatv("{0:2}",V(std::move(rows))).str();}
}
