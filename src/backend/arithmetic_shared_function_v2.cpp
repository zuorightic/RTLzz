#include "backend/arithmetic_shared_function_v2.hpp"
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <tuple>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
[[noreturn]] void fail(const std::string& s){throw std::runtime_error(s);}
std::string text(const O& o,const char* k){auto v=o.getString(k);if(!v)fail(std::string("missing timing field ")+k);return v->str();}
double number(const O& o,const char* k){auto v=o.getNumber(k);if(!v||!std::isfinite(*v)||*v<0)fail(std::string("invalid timing field ")+k);return *v;}
std::string bytes(const std::string& path){auto b=llvm::MemoryBuffer::getFile(path);if(!b)fail("missing timing policy "+path);return (*b)->getBuffer().str();}
bool bit(const std::vector<std::uint64_t>& v,int b){return std::size_t(b/64)<v.size()&&((v[b/64]>>(b%64))&1);}
std::string kind(const Signal& s){
    if(s.value.valid&&s.value.constant)return "constant";
    if(!s.driver)fail("undriven owned timing signal");const auto& d=*s.driver;
    if(d.signed_truncation)fail("signed owned timing wire");
    if(d.kind==OperationKind::PortRead)return "input";
    if(d.kind==OperationKind::AddCarry)return "addcarry";
    if(d.kind==OperationKind::Binary&&d.op==OpCode::Mul)return "mul";
    if(d.kind==OperationKind::Binary&&d.op==OpCode::Add)return "add";
    switch(d.kind){case OperationKind::Assign:return "assign";case OperationKind::Cast:return "cast";
    case OperationKind::ZExt:return "zext";case OperationKind::Trunc:return "trunc";case OperationKind::Slice:return "slice";
    case OperationKind::BitSelect:return "bitselect";case OperationKind::Concat:return "concat";default:fail("operation outside exact owned bit mapping");}
}
struct Bit {std::string kind;NodeId node=0;int bit=0,constant=0;};
struct Arc {
    std::string source_kind,sink_kind;NodeId source=0,sink=0;int operand=0;
    std::vector<std::pair<int,int>> bits;
};
std::vector<Arc> normalized(const Program& p,const ConditionalBoundaryRequest& request,const std::vector<ConditionalNodePrice>& prices){
    std::set<NodeId> arithmetic,inside(request.cut.members.begin(),request.cut.members.end());std::map<NodeId,int> demanded;
    for(const auto& row:prices)if(row.query.stage==request.cut.index){arithmetic.insert(row.query.origin);
        const auto* context=row.query.descriptor.getObject("local_context");demanded[row.query.origin]=int(*context->getObject("producer")->getInteger("result_width"));}
    std::map<NodeId,std::vector<Bit>> cache;std::set<NodeId> visiting;
    std::function<std::vector<Bit>(NodeId)> origins;
    auto operand=[&](const Operand& a){
        if(a.signed_view)fail("signed mapping operand");
        if(a.kind==OperandKind::Symbol)return origins(a.node);
        if(a.kind!=OperandKind::Literal)fail("unknown bit-mapping operand");
        std::vector<Bit> v;for(int b=0;b<a.type.width;++b)v.push_back({"constant",0,0,bit(a.constant.limbs,b)?1:0});return v;
    };
    origins=[&](NodeId id)->std::vector<Bit>{
        if(cache.count(id))return cache.at(id);if(!visiting.insert(id).second)fail("cycle in owned bit graph");
        const auto& s=p.signal(id);if(s.type.isArray()||s.type.width<1||s.type.width>257)fail("invalid owned scalar width");
        std::vector<Bit> result;auto k=kind(s);
        if(k=="constant"){for(int b=0;b<s.type.width;++b)result.push_back({"constant",0,0,bit(s.value.known_one,b)?1:0});}
        else if(request.launches.count(id)||arithmetic.count(id)){
            for(int b=0;b<s.type.width;++b)result.push_back({request.launches.count(id)?"launch":"operator",id,b,0});
        }else{
            if(!inside.count(id)||k=="input"||k=="mul"||k=="add"||k=="addcarry")fail("missing priced launch/operator in normalized graph");
            const auto& d=*s.driver;
            if(k=="concat"){for(std::size_t i=d.operands.size();i-->0;){auto sub=operand(d.operands[i]);result.insert(result.end(),sub.begin(),sub.end());}}
            else {if(d.operands.size()!=1)fail("pure wire has wrong operand count");auto sub=operand(d.operands[0]);
                if(k=="slice"||k=="bitselect"){int lo=k=="slice"?d.lo:d.bit;if(lo<0||lo+s.type.width>int(sub.size()))fail("invalid exact bit selection");result.assign(sub.begin()+lo,sub.begin()+lo+s.type.width);}
                else {result=std::move(sub);result.resize(s.type.width,{"constant",0,0,0});}}
        }
        if(result.size()!=std::size_t(s.type.width))fail("owned bit map width differs");
        for(int b=0;b<s.type.width;++b)if(s.value.valid&&(bit(s.value.known_zero,b)||bit(s.value.known_one,b)))result[b]={"constant",0,0,bit(s.value.known_one,b)?1:0};
        visiting.erase(id);cache.emplace(id,result);return result;
    };
    using Key=std::tuple<std::string,NodeId,std::string,NodeId,int>;std::map<Key,Arc> arcs;
    auto add=[&](const std::vector<Bit>& mapping,const std::string& sink,NodeId to,int input,int relevant){
        for(int b=0;b<std::min(int(mapping.size()),relevant);++b){const auto& origin=mapping[b];if(origin.kind=="constant")continue;
            Key key{origin.kind,origin.node,sink,to,input};auto& edge=arcs[key];edge.source_kind=origin.kind;edge.source=origin.node;edge.sink_kind=sink;edge.sink=to;edge.operand=input;edge.bits.push_back({origin.bit,b});}
    };
    for(auto id:arithmetic){const auto& d=*p.signal(id).driver;for(std::size_t i=0;i<d.operands.size();++i)add(operand(d.operands[i]),"operator",id,int(i),demanded.at(id));}
    for(auto id:request.captures)add(origins(id),"capture",id,-1,p.signal(id).type.width);
    std::vector<Arc> result;for(auto& item:arcs)result.push_back(std::move(item.second));return result;
}
O arcJson(const Arc& arc,double wire){A bits;for(auto [a,b]:arc.bits)bits.push_back(A{a,b});return O{{"source_kind",arc.source_kind},{"source",arc.source},
    {"sink_kind",arc.sink_kind},{"sink",arc.sink},{"operand_index",arc.operand},{"bit_pairs",std::move(bits)},
    {"owner",arc.source_kind=="operator"&&arc.sink_kind=="operator"?"inter_arithmetic":"S_boundary"},
    {"wire_point_ps",arc.source_kind=="operator"&&arc.sink_kind=="operator"?wire:0.}};}
}

struct SharedFunctionCostProviderV2::Impl {
    rtlzz::ArithmeticRequest request;std::string binding,binding_sha,policy_sha;V policy{nullptr};
};
SharedFunctionCostProviderV2::SharedFunctionCostProviderV2(const std::string& binding,const std::string& binding_sha,const rtlzz::ArithmeticRequest& request,
    const std::string& policy,const std::string& policy_sha):impl_(std::make_shared<Impl>()){
    impl_->request=request;impl_->binding=binding;impl_->binding_sha=binding_sha;impl_->policy_sha=policy_sha;
    auto raw=bytes(policy);if(rtlzz::arithmeticHash(raw)!=policy_sha)fail("owned timing policy SHA mismatch");
    auto parsed=llvm::json::parse(raw);if(!parsed){llvm::consumeError(parsed.takeError());fail("invalid owned timing policy JSON");}impl_->policy=std::move(*parsed);auto* b=impl_->policy.getAsObject();
    if(!b||text(*b,"version")!="factored-state-shared-v2"||text(*b,"quality_status")!="candidate_accounting"||b->getBoolean("full_D_ready")!=false||
       text(*b,"operator_model_sha256")!=request.model_bundle_sha256||text(*b,"S_binding_sha256")!=binding_sha)fail("owned timing schema/dependency/qualification mismatch");
    auto limits=b->getArray("target_interval_ps");
    if(!limits||limits->size()!=2)fail("invalid owned timing target interval");
    auto low=(*limits)[0].getAsNumber(),high=(*limits)[1].getAsNumber();
    if(!low||!high||!std::isfinite(*low)||!std::isfinite(*high)||*low<=0||*low>*high||
       !std::isfinite(request.target_period_ps)||request.target_period_ps<*low||request.target_period_ps>*high)fail("owned timing target outside candidate domain");
    const auto* boundaries=b->getObject("boundary_point_ps");const auto* reserve=b->getObject("reserve");if(!boundaries||!reserve)fail("owned timing parameters missing");
    for(auto k:{"input_service->native_state","native_state->native_state","native_state->external_state"})number(*boundaries,k);
    const auto* forward=b->getObject("forward_boundary_point_ps");if(!forward)fail("missing typed forwarding boundary");
    for(auto k:{"input_service->native_state","native_state->native_state","native_state->external_state"})number(*forward,k);
    number(*b,"inter_arithmetic_wire_point_ps");number(*reserve,"joint_reserve_ps");
}

O SharedFunctionCostProviderV2::estimate(const Program& original,const StateCostQuery& state,const std::vector<ConditionalBoundaryRequest>& requests) const {
    O result{{"version","owned-composition-estimate-v1"},{"status","cost_uncovered"},{"timing_policy_sha256",impl_->policy_sha},
        {"operator_model_sha256",impl_->request.model_bundle_sha256},{"S_binding_sha256",impl_->binding_sha},{"full_D_ready",false},{"joint_qualified",false}};
    try{
        ReusedStateBoundaryProvider legacy(impl_->binding,impl_->binding_sha,impl_->request,StateBoundaryUse::CandidateReuse);
        auto old=legacy.price(original,state,requests,impl_->request.model_bundle_sha256);if(!old.covered)fail(old.reason);
        const auto& policy=*impl_->policy.getAsObject();if(text(policy,"state_model_sha256")!=old.state_model_sha256)fail("owned timing S parameter identity mismatch");
        beir::MutableProgram facts(original);facts.ensureValueFacts();auto p=facts.finish();
        ConditionalOperatorCostProvider nodes(original,impl_->request,ConditionalUse::Production);
        auto rows=nodes.placementPrices(original,state.placement,state.internal_stages,requests.empty()?std::vector<NodeId>{}:requests.front().cut.controls);
        std::map<NodeId,std::pair<double,double>> prices;A node_prices;
        for(const auto& row:rows){if(!row.price.covered)fail(row.price.reason);
            if(!prices.emplace(row.query.origin,std::make_pair(row.price.operator_delay_ps,row.price.delay_reserve_ps)).second)fail("arithmetic node charged more than once");
            node_prices.push_back(O{{"node",row.query.origin},{"delay_ps",row.price.operator_delay_ps},{"reserve_ps",row.price.delay_reserve_ps},{"area_um2",row.price.comb_area_um2}});}
        const auto& bounds=*policy.getObject("boundary_point_ps");double wire=number(policy,"inter_arithmetic_wire_point_ps"),margin=number(*policy.getObject("reserve"),"joint_reserve_ps");
        A stages;double violation=0,point_violation=0;
        for(std::size_t i=0;i<requests.size();++i){auto request=requests[i];
            // Real state projection also contains pure forwarding banks that
            // never enter the arithmetic cut. They still consume a boundary.
            for(const auto& slot:state.slots)if(!slot.valid&&slot.variable_bits){
                if(slot.stage==request.cut.index)request.launches.insert(slot.origin);
                if(slot.stage==request.cut.index+1)request.captures.insert(slot.origin);}
            if(request.cut.index==0)for(const auto& port:p.ports)if(port.direction==PortDirection::Input)for(auto n:port.element_nodes)
                if(std::find(request.cut.controls.begin(),request.cut.controls.end(),n)==request.cut.controls.end())request.launches.insert(n);
            for(const auto& signal:p.signals){auto k=kind(signal);if(k=="assign"||k=="cast"||k=="zext"||k=="trunc"||k=="slice"||k=="bitselect"||k=="concat")request.cut.members.push_back(signal.id);}
            auto arcs=normalized(p,request,rows);A connections,captures;
            for(const auto& arc:arcs)connections.push_back(arcJson(arc,wire));
            std::map<NodeId,std::vector<const Arc*>> predecessors,capture_edges;
            for(const auto& arc:arcs)(arc.sink_kind=="operator"?predecessors:capture_edges)[arc.sink].push_back(&arc);
            std::string capture_owner=request.cut.last==request.cut.index?"external_state":"native_state";
            auto boundary=[&](NodeId launch,bool pure){bool native=false;for(const auto& slot:state.slots)if(!slot.external_capture&&!slot.valid&&slot.origin==launch&&slot.stage==request.cut.index)native=true;
                const std::string role=(native?"native_state":"input_service")+std::string("->")+capture_owner;
                return number(pure?*policy.getObject("forward_boundary_point_ps"):bounds,role.c_str());};
            std::map<std::pair<NodeId,bool>,double> values;std::set<NodeId> visiting;
            std::function<double(NodeId,bool)> arrival=[&](NodeId n,bool guard){auto key=std::make_pair(n,guard);if(values.count(key))return values.at(key);
                if(!visiting.insert(n).second)fail("cyclic normalized arithmetic DAG");double maximum=0;
                for(const auto* arc:predecessors[n])maximum=std::max(maximum,arc->source_kind=="launch"?boundary(arc->source,false):arrival(arc->source,guard)+wire);
                if(!prices.count(n))fail("normalized arithmetic node lacks price");auto [d,r]=prices.at(n);visiting.erase(n);return values[key]=maximum+d+(guard?r:0.);};
            for(auto capture:request.captures){double point=0,rawguard=0;
                if(!capture_edges.count(capture))continue; // Entirely proven constant capture has no data timing path.
                for(const auto* edge:capture_edges[capture]){
                    point=std::max(point,edge->source_kind=="launch"?boundary(edge->source,true):arrival(edge->source,false));
                    rawguard=std::max(rawguard,edge->source_kind=="launch"?boundary(edge->source,true):arrival(edge->source,true));}
                const double t=state.period_ps;captures.push_back(O{{"node",capture},{"point_required_period_ps",point},{"point_slack_ps",t-point},
                    {"legacy_node_guard_required_period_ps",rawguard},{"decision_required_period_ps",point+margin},{"decision_slack_ps",t-point-margin},{"joint_reserve_ps",margin}});
                point_violation=std::max(point_violation,point-t);violation=std::max(violation,point+margin-t);}
            violation=std::max(violation,old.stages.at(i).other_timing_violation_ps);
            stages.push_back(O{{"stage",request.cut.index},{"cut_sha256",request.cut.cut_sha256},{"connections",std::move(connections)},{"captures",std::move(captures)},
                {"nonarithmetic_guard_violation_ps",old.stages.at(i).other_timing_violation_ps}});
        }
        auto decoded=llvm::json::parse(old.diagnostics_json);
        if(!decoded||!decoded->getAsObject())fail("missing actual S semantic projection diagnostics");
        const auto& diagnostics=*decoded->getAsObject();const auto* features=diagnostics.getObject("owner_features");
        if(!features)fail("missing actual state owner features");
        std::map<std::string,double> shared_features{{"one",1},{"input_data_bits",number(*features->getObject("input_service"),"data_bits")},
            {"total_FF_bits",number(diagnostics,"total_FF_bits")},{"reset_one_bits",0},{"predicted_functional_area_um2",0}};
        O owners;double area=0;
        for(auto n:{"input_service","native_state","external_state"}){
            auto value=old.owner_area_um2.at(n);owners[n]=value;area+=value;
            const auto* f=features->getObject(n);if(!f)fail("missing unique S owner");shared_features["reset_one_bits"]+=number(*f,"reset_one_bits");}
        double arithmetic_area=0;for(const auto& row:rows)arithmetic_area+=row.price.comb_area_um2;owners["operators"]=arithmetic_area;area+=arithmetic_area;
        shared_features["predicted_functional_area_um2"]=area;
        shared_features["sqrt_FF_bits"]=std::sqrt(shared_features.at("total_FF_bits"));
        shared_features["input_ports"]=number(*features->getObject("input_service"),"data_signals");
        shared_features["clock_pressure"]=1500./state.period_ps;
        const auto* shared_models=policy.getObject("shared_area_models");if(!shared_models||shared_models->size()!=3)fail("missing factored shared area model inventory");
        for(auto owner:{"shared_clock","shared_physical","root_repair"}){
            const auto* model=shared_models->getObject(owner);if(!model||text(*model,"kind")!="nonnegative_linear")fail("invalid simple shared area model");
            const auto* names=model->getArray("features");const auto* coefficients=model->getArray("coefficients");const auto* support=model->getObject("numeric_support");
            if(!names||!coefficients||names->size()!=coefficients->size()||!support||support->size()!=names->size())fail("shared feature/coefficient/domain shape mismatch");
            std::set<std::string> seen;double value=0;
            for(std::size_t j=0;j<names->size();++j){
                auto name=(*names)[j].getAsString();auto co=(*coefficients)[j].getAsNumber();
                if(!name||!shared_features.count(name->str())||!seen.insert(name->str()).second||!co||!std::isfinite(*co)||*co<0)fail("invalid prediction-time shared feature or coefficient");
                const auto* range=support->getArray(name->str());if(!range||range->size()!=2)fail("invalid shared numeric domain");
                auto low=(*range)[0].getAsNumber(),high=(*range)[1].getAsNumber();double feature=shared_features.at(name->str());
                if(!low||!high||!std::isfinite(*low)||!std::isfinite(*high)||*low>*high||feature<*low-1e-10*std::max({1.,std::abs(*low),std::abs(*high)})||feature>*high+1e-10*std::max({1.,std::abs(*low),std::abs(*high)}))fail("shared feature outside frozen support: "+name->str());
                value+=*co*feature;
            }
            if(!std::isfinite(value)||value<0)fail("invalid shared area prediction");owners[owner]=value;area+=value;
        }
        O shared_json;for(auto [name,value]:shared_features)shared_json[name]=value;result["shared_features"]=std::move(shared_json);
        result["status"]="candidate_covered";result["stages"]=std::move(stages);result["node_prices"]=std::move(node_prices);result["owner_area_um2"]=std::move(owners);result["total_area_um2"]=area;
        result["decision_violation_ps"]=violation;result["point_violation_ps"]=point_violation;result["G_delay_ps"]=0.;result["G_area_um2"]=0.;
        result["layout_sha256"]=state.layout_sha256;result["target_ps"]=state.period_ps;result["state_model_sha256"]=old.state_model_sha256;
        result["nonarithmetic_policy"]="unchanged original S control/service/bypass guards";
    }catch(const std::exception& error){result["reason"]=std::string(error.what());}
    return result;
}

O sharedFunctionGraphJsonV2(const Program& original){beir::MutableProgram facts(original);facts.ensureValueFacts();auto p=facts.finish();A nodes;
    for(const auto& s:p.signals){A operands,zeros,ones;for(int b=0;b<s.type.width;++b){if(s.value.valid&&bit(s.value.known_zero,b))zeros.push_back(b);if(s.value.valid&&bit(s.value.known_one,b))ones.push_back(b);}
        if(s.driver)for(const auto& operand:s.driver->operands){A literal;for(int b=0;b<operand.type.width;++b)literal.push_back(bit(operand.constant.limbs,b)?1:0);
            O o{{"width",operand.type.width},{"signed",operand.signed_view},{"kind",operand.kind==OperandKind::Symbol?"symbol":operand.kind==OperandKind::Literal?"literal":"port"}};
            if(operand.kind==OperandKind::Symbol)o["node"]=operand.node;else if(operand.kind==OperandKind::Literal)o["bits"]=std::move(literal);operands.push_back(std::move(o));}
        nodes.push_back(O{{"id",s.id},{"width",s.type.width},{"kind",kind(s)},{"known_zero",std::move(zeros)},{"known_one",std::move(ones)},
            {"lo",s.driver?s.driver->lo:0},{"hi",s.driver?s.driver->hi:0},{"bit",s.driver?s.driver->bit:0},{"operands",std::move(operands)}});}
    return O{{"version","owned-bit-graph-v1"},{"graph_sha256",rtlzz::arithmeticHash(emitText(original))},{"nodes",std::move(nodes)}};
}
}
