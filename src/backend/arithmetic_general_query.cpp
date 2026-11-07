#include "backend/arithmetic_general_state.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <functional>
#include <iomanip>
#include <set>
#include <sstream>

namespace pred::beir::arithmetic {
namespace {
[[noreturn]] void fail(const std::string& reason){throw rtlzz::ArithmeticError({"general_state_query","cost_uncovered",reason,{},{}});}
bool bit(const std::vector<std::uint64_t>& a,int b){return std::size_t(b/64)<a.size()&&((a[b/64]>>(b%64))&1);}
std::uint64_t ones(const StateSlot& s){std::uint64_t n=0;for(int b=0;b<s.declared_width;++b)n+=bit(s.variable_mask,b)&&bit(s.reset_values,b);return n;}
struct Affine {
    bool supported=true,invert=false;
    std::set<std::pair<NodeId,int>> roots;
};
// This recognizer is deliberately bounded by the published two-root affine
// source family. Arithmetic/calls/Muxes do not acquire source qualification.
struct Relations {
    const Program& p;std::set<NodeId> inputs;std::map<std::pair<NodeId,int>,Affine> cache;
    std::set<std::pair<NodeId,int>> visiting;
    Affine operand(const Operand& a,int b){
        if(b<0||b>=a.type.width)return {};
        if(a.kind==OperandKind::Literal){Affine e;e.invert=bit(a.constant.limbs,b);return e;}
        if(a.kind!=OperandKind::Symbol)return {false,false,{}};
        return get(a.node,b);
    }
    Affine get(NodeId id,int b){
        auto key=std::make_pair(id,b);if(auto it=cache.find(key);it!=cache.end())return it->second;
        if(!visiting.insert(key).second)fail("cyclic general state source");
        const auto& s=p.signal(id);if(!s.driver)fail("undriven general state source");
        const auto& d=*s.driver;Affine e{false,false,{}};
        if(b<0||b>=s.type.width)e={};
        else if(d.kind==OperationKind::PortRead&&inputs.count(id))e={true,false,{{id,b}}};
        else if(s.value.valid&&s.value.constant)e={true,bit(s.value.value.limbs,b),{}};
        else if((d.kind==OperationKind::Assign||d.kind==OperationKind::Cast||d.kind==OperationKind::Trunc||d.kind==OperationKind::ZExt)&&
                d.operands.size()==1&&!d.signed_truncation&&!d.operands[0].signed_view)e=operand(d.operands[0],b);
        else if(d.kind==OperationKind::Slice&&d.operands.size()==1)e=operand(d.operands[0],b+d.lo);
        else if(d.kind==OperationKind::Concat){int offset=0;for(auto it=d.operands.rbegin();it!=d.operands.rend();++it){
            if(b>=offset&&b<offset+it->type.width){e=operand(*it,b-offset);break;}offset+=it->type.width;}}
        else if(d.kind==OperationKind::Unary&&d.op==OpCode::BitNot&&d.operands.size()==1){e=operand(d.operands[0],b);e.invert=!e.invert;}
        else if(d.kind==OperationKind::Binary&&d.op==OpCode::BitXor&&d.operands.size()==2){
            auto a=operand(d.operands[0],b),c=operand(d.operands[1],b);e={a.supported&&c.supported,a.invert!=c.invert,a.roots};
            for(auto root:c.roots){if(e.roots.count(root))e.roots.erase(root);else e.roots.insert(root);}e.supported&=e.roots.size()<=2;
        }
        visiting.erase(key);return cache.emplace(key,std::move(e)).first->second;
    }
    std::pair<bool,std::vector<std::string>> word(const StateSlot& slot){
        bool supported=true;std::vector<std::string> terms;
        for(int b=0;b<slot.declared_width;++b){
            auto e=get(slot.origin,b);std::ostringstream value;
            if(bit(slot.variable_mask,b)){
                if(!e.supported||e.roots.size()!=2)supported=false;
                value<<(e.invert?'1':'0');for(auto [id,k]:e.roots)value<<'/'<<id<<':'<<k;
            }else if(!e.supported||!e.roots.empty()||e.invert||bit(slot.reset_values,b))supported=false;
            terms.push_back(value.str());
        }return {supported,terms};
    }
};
std::string semantic(const GeneralStateCostQuery& q){
    std::ostringstream s;s<<std::setprecision(17)<<q.core.graph_sha256<<'/'<<q.core.layout_sha256<<'/'<<q.core.period_ps<<'/'<<q.protocol.mode
        <<'/'<<q.protocol.valid_write_enable<<'/'<<q.protocol.external_data_unconditional<<'/'<<q.protocol.external_valid_unconditional;
    for(const auto& [port,en]:q.protocol.data_write_enables)s<<'/'<<port<<':'<<en;
    for(const auto& slot:q.slots){const auto& v=slot.state;
        s<<'/'<<slot.owner<<'/'<<v.origin<<'/'<<v.stage<<'/'<<v.enable_stage<<'/'<<v.valid<<'/'<<v.external_capture
            <<'/'<<v.declared_width<<'/'<<v.variable_bits<<'/'<<slot.write_enable<<'/'<<slot.affine_supported;
        for(auto x:v.variable_mask)s<<'/'<<x;for(auto x:v.reset_values)s<<'/'<<x;
        for(const auto& term:slot.affine_bits)s<<'/'<<term;
    }
    for(const auto& [owner,features]:q.owner_features)for(const auto& [name,value]:features)s<<'/'<<owner<<'/'<<name<<':'<<value;
    for(const auto& [name,value]:q.control_features)s<<'/'<<name<<':'<<value;
    s<<'/'<<q.source_scope_qualified<<'/'<<q.source_scope;return s.str();
}
GeneralStateCostQuery assemble(const Program& original,StateCostQuery core,const ServiceStateProtocol& protocol){
    if(protocol.mode<0||protocol.mode>2||!protocol.external_data_unconditional||!protocol.external_valid_unconditional)
        fail("general state requires explicit service enables and unconditional external data/valid capture");
    auto identifier=[](const std::string& x){return !x.empty()&&(std::isalpha(static_cast<unsigned char>(x.front()))||x.front()=='_')&&
        std::all_of(x.begin(),x.end(),[](unsigned char c){return std::isalnum(c)||c=='_';});};
    for(const auto& [name,en]:protocol.data_write_enables)if(!identifier(en))fail("general state write enable must be a dynamic scalar identifier");
    if(!protocol.valid_write_enable.empty()&&!identifier(protocol.valid_write_enable))fail("general state input-valid enable must be a dynamic scalar identifier");
    MutableProgram facts(original);facts.ensureValueFacts();auto p=facts.finish();
    GeneralStateCostQuery q;q.core=std::move(core);q.protocol=protocol;
    std::set<NodeId> input_data;std::set<std::string> expected_ports,enables;
    pipeline::Program reset;reset.combinational=p;reset.rtl_program=p;reset.options=q.core.projection_options;
    std::vector<std::pair<NodeId,std::string>> service;
    for(const auto& port:p.ports)if(port.direction==PortDirection::Input){
        if(port.type.isArray()||port.element_nodes.size()!=1||port.type.width<1)fail("general state service requires scalar port words");
        auto id=port.element_nodes.front();service.emplace_back(id,port.name);reset.registers.push_back({id,id,0});
        if(port.name!=q.core.projection_options.input_valid_name){
            expected_ports.insert(port.name);input_data.insert(id);auto it=protocol.data_write_enables.find(port.name);
            if(it==protocol.data_write_enables.end()||it->second.empty())fail("general state input port lacks a write-enable owner");
            if(protocol.mode==1&&it->second!=protocol.valid_write_enable)fail("general state shared service enable mismatch");
            if(protocol.mode!=1&&!enables.insert(it->second).second)fail("general state independent service enables alias");
        }
    }
    std::set<std::string> actual_ports;for(const auto& [name,en]:protocol.data_write_enables)actual_ports.insert(name);
    if(actual_ports!=expected_ports||service.empty()||
       (protocol.mode==2?!protocol.valid_write_enable.empty():protocol.valid_write_enable.empty())||
       (protocol.mode==0&&enables.count(protocol.valid_write_enable)))fail("general state service valid protocol/port membership mismatch");
    pipeline::initializeResetValues(reset);std::map<NodeId,std::vector<std::uint64_t>> reset_words;
    for(const auto& r:reset.registers)reset_words[r.node]=r.reset_values;
    Relations relations{p,input_data,{}, {}};
    for(auto [id,name]:service){bool valid=name==q.core.projection_options.input_valid_name;
        StateSlot slot;slot.origin=id;slot.stage=0;slot.enable_stage=protocol.mode==2&&valid?-1:-2;
        slot.declared_width=p.signal(id).type.width;slot.valid=valid;slot.name=name;slot.reset_values=reset_words.at(id);
        slot.variable_mask.assign((slot.declared_width+63)/64,UINT64_MAX);
        if(slot.declared_width%64)slot.variable_mask.back()=(std::uint64_t(1)<<(slot.declared_width%64))-1;
        slot.variable_bits=slot.declared_width;
        q.slots.push_back({slot,"input_service",valid?protocol.valid_write_enable:protocol.data_write_enables.at(name),{},true});
    }
    q.source_scope_qualified=true;
    for(auto slot:q.core.slots){std::string owner=slot.external_capture?"external_state":"native_state";
        std::string enable=slot.external_capture||slot.valid?"":("valid_cut_"+std::to_string(slot.enable_stage));
        if(slot.external_capture)slot.enable_stage=-1;
        auto affine=slot.valid?std::make_pair(true,std::vector<std::string>{}):relations.word(slot);
        q.source_scope_qualified&=affine.first;
        q.slots.push_back({std::move(slot),owner,enable,std::move(affine.second),affine.first});
    }
    q.source_scope=q.source_scope_qualified?"recognized_two_root_affine_native_and_external":"arbitrary_or_single_root_source_requires_joint_G";
    std::map<int,std::pair<double,double>> cut_load;
    for(const auto& item:q.slots){const auto& slot=item.state;auto& f=q.owner_features[item.owner];
        f["data_bits"]+=slot.valid?0:slot.variable_bits;f["data_signals"]+=!slot.valid&&slot.variable_bits>0;
        f["valid_bits"]+=slot.valid?slot.variable_bits:0;f["reset_one_bits"]+=slot.valid?0:ones(slot);
        if(item.owner=="native_state"&&!slot.valid){auto& load=cut_load[slot.stage];load.first+=slot.variable_bits;load.second+=slot.variable_bits>0;}
    }
    for(const auto& owner:{"input_service","native_state","external_state"}){
        auto& f=q.owner_features[owner];f["state_bits"]=f["data_bits"]+f["valid_bits"];
        f["state_signals"]=f["data_signals"]+f["valid_bits"];
        f["enable_domains"]=std::string(owner)=="input_service"?(protocol.mode==1?1:input_data.size()+int(protocol.mode!=2)):
            std::string(owner)=="native_state"?cut_load.size():0;
        f["sdc_period_ps"]=q.core.period_ps;
    }
    double max_bits=0,max_words=0;for(auto [stage,load]:cut_load){max_bits=std::max(max_bits,load.first);max_words=std::max(max_words,load.second);}
    const auto& n=q.owner_features.at("native_state");
    q.control_features={{"one",1},{"native_enable_bits_max",max_bits},{"native_enable_bits_sqrt",std::sqrt(max_bits)},
        {"native_enable_bits_log2",std::log2(1+max_bits)},{"native_enable_words_max",max_words},
        {"native_valid_targets_max",max_bits+2},{"native_data_bits",n.at("data_bits")},
        {"native_reset_one_bits",n.at("reset_one_bits")},{"input_data_bits",q.owner_features.at("input_service").at("data_bits")},
        {"external_data_bits",q.owner_features.at("external_state").at("data_bits")}};
    q.layout_sha256=rtlzz::arithmeticHash(semantic(q));return q;
}
}
// Used by the price consumer to compare every caller-visible field against a
// rebuilt query. Names/materialization flags are evidence, not numeric inputs.
std::string generalStateSemanticFingerprint(const GeneralStateCostQuery& q){return rtlzz::arithmeticHash(semantic(q));}
GeneralStateCostQuery generalStateCostQuery(const Program& p,const pipeline::Options& options,const std::vector<int>& stages,
    double period,const ServiceStateProtocol& protocol){return assemble(p,stateCostQuery(p,options,stages,period,true),protocol);}
GeneralStateCostQuery generalStateCostQuery(const pipeline::Program& p,double period,const ServiceStateProtocol& protocol){
    return assemble(p.combinational,stateCostQuery(p,period,true),protocol);
}
std::string generalStateCostQueryJson(const GeneralStateCostQuery& q){
    using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
    auto words=[](const auto& v){A a;for(auto x:v){std::ostringstream s;s<<std::hex<<x;a.push_back(s.str());}return a;};
    auto map=[](const auto& values){O out;for(const auto& [k,v]:values)out[k]=v;return out;};
    A slots;for(const auto& i:q.slots){const auto& s=i.state;A affine;for(const auto& x:i.affine_bits)affine.push_back(x);
        slots.push_back(O{{"owner",i.owner},{"origin",s.origin},{"stage",s.stage},{"valid",s.valid},{"name",s.name},
            {"declared_width",s.declared_width},{"variable_bits",s.variable_bits},{"variable_mask_hex",words(s.variable_mask)},
            {"reset_hex",words(s.reset_values)},{"enable_stage",s.enable_stage},{"write_enable",i.write_enable},
            {"external_capture",s.external_capture},{"affine_supported",i.affine_supported},{"affine_bits",std::move(affine)}});}
    O owners;for(const auto& [o,f]:q.owner_features)owners[o]=map(f);
    return llvm::formatv("{0:2}",V(O{{"version","general-state-query-v1"},{"layout_sha256",q.layout_sha256},
        {"graph_sha256",q.core.graph_sha256},{"period_ps",q.core.period_ps},{"internal_stages",q.core.internal_stages},
        {"materialized_checked",q.core.materialized_checked},{"service_mode",q.protocol.mode},
        {"service_write_enables",map(q.protocol.data_write_enables)},{"service_valid_write_enable",q.protocol.valid_write_enable},
        {"external_data_unconditional",q.protocol.external_data_unconditional},{"external_valid_unconditional",q.protocol.external_valid_unconditional},
        {"owner_features",std::move(owners)},{"control_features",map(q.control_features)},
        {"source_scope_qualified",q.source_scope_qualified},{"source_scope",q.source_scope},{"slots",std::move(slots)},
        {"joint_arithmetic_boundary_qualified",false},{"PPA_calls",0}})).str();
}
std::string generalStateCostEstimateJson(const GeneralStateCostEstimate& p){
    using O=llvm::json::Object;auto map=[](const auto& values){O o;for(const auto& [k,v]:values)o[k]=v;return o;};
    return llvm::formatv("{0:2}",llvm::json::Value(O{{"owner_area_um2",map(p.owner_area_um2)},
        {"owned_state_area_um2",p.owned_state_area_um2},{"boundary_upper_ps",map(p.boundary_upper_ps)},
        {"residual_budget_ps",map(p.residual_budget_ps)},{"bits",p.bits},{"signals",p.signals},
        {"model_sha256",p.model_sha256},{"layout_sha256",p.layout_sha256},
        {"shared_physical_area_included",p.shared_physical_area_included},
        {"joint_arithmetic_boundary_qualified",p.joint_arithmetic_boundary_qualified}})).str();
}
}
