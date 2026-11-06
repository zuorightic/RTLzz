#include "backend/arithmetic_state.hpp"
#include "backend/rtl_names.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <sstream>

namespace pred::beir::arithmetic {
namespace {
[[noreturn]] void fail(const std::string& why) {
    throw rtlzz::ArithmeticError({"state_query","cost_uncovered",why,{},{}});
}
std::vector<std::uint64_t> mask(const Signal& s) {
    if(s.type.isArray()||s.type.width<=0)fail("native state query requires scalar positive-width words");
    std::vector<std::uint64_t> result((s.type.width+63)/64,0);
    for(int bit=0;bit<s.type.width;++bit) {
        auto word=std::size_t(bit/64);auto flag=std::uint64_t(1)<<(bit%64);
        bool known=s.value.valid&&((word<s.value.known_zero.size()&&(s.value.known_zero[word]&flag))||
                                  (word<s.value.known_one.size()&&(s.value.known_one[word]&flag)));
        if(!(s.value.valid&&s.value.constant)&&!known)result[word]|=flag;
    }
    return result;
}
std::uint64_t popcount(const std::vector<std::uint64_t>& words) {
    std::uint64_t result=0;for(auto word:words)while(word){word&=word-1;++result;}return result;
}
void add(std::uint64_t& total,std::uint64_t value) {
    if(value>UINT64_MAX-total)fail("native state count overflow");total+=value;
}
std::string key(NodeId origin,int stage,bool valid,bool external) {
    return std::to_string(origin)+"/"+std::to_string(stage)+"/"+std::to_string(valid)+"/"+std::to_string(external);
}
void finalize(StateCostQuery& q) {
    std::uint64_t data_bits=0,data_signals=0,valid_bits=0,valid_signals=0,reset_ones=0;
    int maximum=0,minimum=std::numeric_limits<int>::max();std::set<int> enables;
    std::ostringstream fingerprint;fingerprint<<q.graph_sha256<<'/'<<q.internal_stages<<'/'<<q.external_outputs_included;
    for(const auto& slot:q.slots) {
        if(slot.reset_values.size()!=slot.variable_mask.size())fail("state reset/mask limb shape mismatch");
        fingerprint<<'/'<<key(slot.origin,slot.stage,slot.valid,slot.external_capture)<<'/'<<slot.declared_width<<'/'<<slot.enable_stage;
        std::vector<std::uint64_t> ones(slot.variable_mask.size());
        for(std::size_t i=0;i<ones.size();++i){ones[i]=slot.reset_values[i]&slot.variable_mask[i];fingerprint<<'/'<<slot.variable_mask[i]<<':'<<slot.reset_values[i];}
        if(slot.valid){add(valid_bits,slot.variable_bits);if(slot.variable_bits)++valid_signals;add(q.control_reset_one_bits,popcount(ones));}
        else if(slot.variable_bits){add(data_bits,slot.variable_bits);++data_signals;add(reset_ones,popcount(ones));
            maximum=std::max(maximum,slot.declared_width);minimum=std::min(minimum,int(slot.variable_bits));enables.insert(slot.enable_stage);}
    }
    q.features={{"state_bits",double(data_bits+valid_bits)},{"state_signals",double(data_signals+valid_signals)},
        {"data_bits",double(data_bits)},{"data_signals",double(data_signals)},
        {"valid_bits",double(valid_bits)},{"valid_signals",double(valid_signals)},
        {"enable_domains",double(enables.size())},{"reset_one_bits",double(reset_ones)},
        {"max_declared_width",double(maximum)},{"min_variable_width",double(data_signals?minimum:0)},
        {"sdc_period_ps",q.period_ps}};
    q.layout_sha256=rtlzz::arithmeticHash(fingerprint.str());
}
}

StateCostQuery stateCostQuery(const Program& original,const pipeline::Options& options,
                             const std::vector<int>& placement,double period_ps,bool external_outputs) {
    pipeline::validateOptions(options);
    if(!options.reset_enabled||!options.reset_active_low||options.input_valid_name.empty())
        fail("native state query requires synchronous active-low reset and explicit valid chain");
    if(!std::isfinite(period_ps)||period_ps<=0||placement.size()!=original.signals.size())fail("invalid state placement/target");
    beir::MutableProgram facts(original);facts.ensureValueFacts();auto p=facts.finish();
    StateCostQuery q;q.period_ps=period_ps;q.internal_stages=options.register_stages;q.external_outputs_included=external_outputs;
    q.projection_options=options;q.placement=placement;
    q.graph_sha256=rtlzz::arithmeticHash(emitText(p));
    NodeId iv=kInvalidNodeId,ov=kInvalidNodeId;
    std::set<NodeId> input,output,live;std::vector<NodeId> pending;
    for(const auto& port:p.ports) {
        if(port.name==options.input_valid_name||port.name==options.output_valid_name) {
            bool is_input=port.name==options.input_valid_name;
            if(port.direction!=(is_input?PortDirection::Input:PortDirection::Output)||port.type.width!=1||port.type.isArray()||port.element_nodes.size()!=1)
                fail("native state valid ports do not match the pipeline protocol");
            (is_input?iv:ov)=port.element_nodes.front();
        }
        for(auto id:port.element_nodes) {
            (port.direction==PortDirection::Input?input:output).insert(id);pending.push_back(id);
            if(port.direction==PortDirection::Input)add(q.input_variable_bits,popcount(mask(p.signal(id))));
        }
    }
    if(iv==kInvalidNodeId||ov==kInvalidNodeId||iv==ov)fail("native state query lacks distinct valid input/output");
    q.valid_input_rtl_name=rtl_names::sanitizeIdentifier(p.signal(iv).name);
    for(const auto& region:p.arithmetic_regions){pending.insert(pending.end(),region.outputs.begin(),region.outputs.end());for(const auto& a:region.inputs)if(a.kind==OperandKind::Symbol)pending.push_back(a.node);}
    std::vector<std::vector<NodeId>> users(p.signals.size());
    while(!pending.empty()) {
        auto id=pending.back();pending.pop_back();if(!live.insert(id).second)continue;
        const auto& s=p.signal(id);if(!s.driver)fail("native state has an undriven live node");
        for(const auto& a:s.driver->operands)if(a.kind==OperandKind::Symbol){users[a.node].push_back(id);pending.push_back(a.node);}
    }
    std::vector<int> constant(p.signals.size(),-1),visiting(p.signals.size());
    std::function<bool(NodeId)> is_constant=[&](NodeId id) {
        if(constant[id]>=0)return bool(constant[id]);if(visiting[id]++)fail("cyclic state graph");
        const auto& d=*p.signal(id).driver;bool result=!input.count(id)&&d.kind!=OperationKind::PortRead&&d.kind!=OperationKind::Call;
        for(const auto& a:d.operands)if(a.kind!=OperandKind::Literal&&(a.kind!=OperandKind::Symbol||!is_constant(a.node)))result=false;
        --visiting[id];constant[id]=result;return result;
    };
    std::set<NodeId> origins;
    auto slot=[&](NodeId id,int stage,bool valid,bool external,const std::string& name) {
        StateSlot s;s.origin=id;s.stage=stage;s.valid=valid;s.external_capture=external;s.name=name;
        const auto& signal=p.signal(id);s.declared_width=signal.type.width;
        s.variable_mask=valid?std::vector<std::uint64_t>{1}:mask(signal);s.variable_bits=popcount(s.variable_mask);
        s.enable_stage=valid?-1:stage-1;q.slots.push_back(std::move(s));origins.insert(id);
    };
    for(auto id:live) {
        if(placement[id]<0||placement[id]>options.register_stages||(input.count(id)&&placement[id]!=0)||(output.count(id)&&placement[id]!=options.register_stages))
            fail("native state query has invalid placement");
        for(auto user:users[id])if(placement[user]<placement[id])fail("native state placement reverses a dependency");
        if(id==iv||id==ov||is_constant(id))continue;
        int end=placement[id];for(auto user:users[id])end=std::max(end,placement[user]);
        for(int stage=placement[id]+1;stage<=end;++stage)slot(id,stage,false,false,p.signal(id).name);
    }
    for(int stage=1;stage<=options.register_stages;++stage)slot(iv,stage,true,false,"valid");
    if(external_outputs)for(const auto& port:p.ports)if(port.direction==PortDirection::Output)
        for(auto id:port.element_nodes)slot(id==ov?iv:id,options.register_stages+1,id==ov,true,port.name);
    pipeline::Program reset_view;reset_view.combinational=p;reset_view.rtl_program=p;reset_view.options=options;
    for(auto id:origins)reset_view.registers.push_back({id,id,0});
    pipeline::initializeResetValues(reset_view);
    std::map<NodeId,std::vector<std::uint64_t>> resets;
    for(const auto& reg:reset_view.registers)resets[reg.node]=reg.reset_values;
    for(auto& s:q.slots)s.reset_values=resets.at(s.origin);
    finalize(q);return q;
}

StateCostQuery stateCostQuery(const pipeline::Program& actual,double period_ps,bool external_outputs) {
    pipeline::validateProgram(actual);
    auto q=stateCostQuery(actual.combinational,actual.options,actual.stages,period_ps,external_outputs);
    std::map<std::string,const pipeline::Register*> seen;
    std::map<NodeId,NodeId> origins;for(const auto& signal:actual.combinational.signals)origins[signal.id]=signal.id;
    NodeId valid_origin=actual.valid_nodes.front();
    for(const auto& r:actual.registers) {
        auto found=origins.find(r.source);if(found==origins.end())fail("state source is not a prior logical value");
        NodeId origin=r.is_valid?valid_origin:found->second;origins[r.node]=origin;
        const auto identity=key(origin,r.stage,r.is_valid,false);
        if(!seen.emplace(identity,&r).second)fail("duplicate physical state slot");
    }
    for(auto& slot:q.slots)if(!slot.external_capture) {
        auto at=seen.find(key(slot.origin,slot.stage,slot.valid,false));
        if(at==seen.end())fail("projected state slot was not materialized");
        const auto& reg=*at->second;
        if(reg.reset_values!=slot.reset_values||actual.rtl_program.signal(reg.node).type.width!=slot.declared_width)
            fail("materialized state reset/type differs from the price query");
        if(!slot.valid&&reg.enable!=actual.valid_nodes.at(slot.enable_stage))fail("materialized state enable differs from the price query");
        slot.name=actual.rtl_program.signal(reg.node).name;seen.erase(at);
    }
    if(!seen.empty())fail("materialized pipeline has unaccounted state");
    q.materialized_checked=true;return q;
}

std::string stateCostQueryJson(const StateCostQuery& q) {
    using O=llvm::json::Object;using A=llvm::json::Array;
    auto words=[](const std::vector<std::uint64_t>& values){A out;for(auto v:values){std::ostringstream s;s<<std::hex<<v;out.push_back(s.str());}return out;};
    A slots;for(const auto& s:q.slots)slots.push_back(O{{"origin",s.origin},{"stage",s.stage},{"valid",s.valid},
        {"external_capture",s.external_capture},{"name",s.name},{"declared_width",s.declared_width},
        {"variable_bits",s.variable_bits},{"variable_mask_hex",words(s.variable_mask)},
        {"reset_hex",words(s.reset_values)},{"enable_stage",s.enable_stage}});
    O features;for(const auto& [name,value]:q.features)features[name]=value;
    return llvm::formatv("{0:2}",llvm::json::Value(O{{"version","native-state-query-v1"},
        {"feature_schema","arithmetic-state-bank-features-v1"},{"features",std::move(features)},
        {"slots",std::move(slots)},{"graph_sha256",q.graph_sha256},{"layout_sha256",q.layout_sha256},
        {"internal_stages",q.internal_stages},{"materialized_checked",q.materialized_checked},
        {"valid_input_rtl_name",q.valid_input_rtl_name},
        {"external_outputs_included",q.external_outputs_included},{"input_variable_bits",q.input_variable_bits},
        {"control_reset_one_bits",q.control_reset_one_bits},{"state_scope","core internal plus explicit external output captures; excludes input/service wrapper state"},
        {"enable_policy","per-stage previous valid; stage0 is the external valid input"},
        {"qualified_price",false},{"needs_native_protocol_qualification",true},{"PPA_calls",0}})).str();
}
}
