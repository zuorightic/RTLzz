#include "backend/arithmetic_fused_cost.hpp"
#include <llvm/Support/MemoryBuffer.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <tuple>
namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
void require(bool b,const std::string& why){if(!b)throw std::runtime_error(why);}
double number(const O& o,const std::string& k){auto n=o.getNumber(k);require(n&&std::isfinite(*n)&&*n>=0,"invalid G/S field "+k);return *n;}
std::string text(const O& o,const char* k){auto s=o.getString(k);require(bool(s),std::string("missing G/S string ")+k);return s->str();}
[[noreturn]] void fail(const std::string& why){throw std::runtime_error(why);}
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
struct BitArc {
    std::string source_kind,sink_kind;NodeId source=0,sink=0;int operand=0;
    std::vector<std::pair<int,int>> bits;
};
std::vector<BitArc> normalized(const Program& p,const ConditionalBoundaryRequest& request,const std::vector<ConditionalNodePrice>& prices){
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
    using Key=std::tuple<std::string,NodeId,std::string,NodeId,int>;std::map<Key,BitArc> arcs;
    auto add=[&](const std::vector<Bit>& mapping,const std::string& sink,NodeId to,int input,int relevant){
        for(int b=0;b<std::min(int(mapping.size()),relevant);++b){const auto& origin=mapping[b];if(origin.kind=="constant")continue;
            Key key{origin.kind,origin.node,sink,to,input};auto& edge=arcs[key];edge.source_kind=origin.kind;edge.source=origin.node;edge.sink_kind=sink;edge.sink=to;edge.operand=input;edge.bits.push_back({origin.bit,b});}
    };
    for(auto id:arithmetic){const auto& d=*p.signal(id).driver;for(std::size_t i=0;i<d.operands.size();++i)add(operand(d.operands[i]),"operator",id,int(i),demanded.at(id));}
    for(auto id:request.captures)add(origins(id),"capture",id,-1,p.signal(id).type.width);
    std::vector<BitArc> result;for(auto& item:arcs)result.push_back(std::move(item.second));return result;
}
O arcJson(const BitArc& arc,double wire){A bits;for(auto [a,b]:arc.bits)bits.push_back(A{a,b});return O{{"source_kind",arc.source_kind},{"source",arc.source},
    {"sink_kind",arc.sink_kind},{"sink",arc.sink},{"operand_index",arc.operand},{"bit_pairs",std::move(bits)},
    {"owner",arc.source_kind=="operator"&&arc.sink_kind=="operator"?"inter_arithmetic":"S_boundary"},
    {"wire_point_ps",arc.source_kind=="operator"&&arc.sink_kind=="operator"?wire:0.}};}
struct Arc {std::string from_kind,to_kind;NodeId from,to;};
}
FusedFunctionCostProvider::FusedFunctionCostProvider(const rtlzz::ArithmeticRequest& r,const FusedDContext& c):
    state_(c.S_binding,c.S_binding_sha256,r,StateBoundaryUse::CandidateReuse),
    regions_(c.G_model,c.G_model_sha256,r.platform_profile_sha256),request_(r),context_(c){}

O FusedFunctionCostProvider::estimate(const Program& p,const StateCostQuery& state,const std::vector<ConditionalBoundaryRequest>& cuts) const {
    O out{{"version","fused-function-composition-v1"},{"status","cost_uncovered"},{"G_model_sha256",context_.G_model_sha256},{"reduction_policy",kReductionPolicy},
        {"operator_model_sha256",request_.model_bundle_sha256},{"S_binding_sha256",context_.S_binding_sha256},{"timing_policy_sha256",context_.composition_policy_sha256},
        {"layout_sha256",state.layout_sha256},{"target_ps",state.period_ps},{"full_D_ready",false},{"joint_qualified",false}};
    try{
        const auto regions=reductionRegions(p,state.placement,state.internal_stages,cuts.empty()?std::vector<NodeId>{}:cuts.front().cut.controls);
        auto file=llvm::MemoryBuffer::getFile(context_.composition_policy);require(bool(file),"missing shared policy");
        require(rtlzz::arithmeticHash((*file)->getBuffer().str())==context_.composition_policy_sha256,"shared policy drift");auto parsed=llvm::json::parse((*file)->getBuffer());if(!parsed)throw std::runtime_error(llvm::toString(parsed.takeError()));require(parsed->getAsObject(),"invalid shared policy");const auto& policy=*parsed->getAsObject();
        require(policy.getString("version")=="factored-state-shared-v2"&&policy.getString("quality_status")=="candidate_accounting"&&policy.getBoolean("full_D_ready")==false&&policy.getString("operator_model_sha256")==request_.model_bundle_sha256&&policy.getString("S_binding_sha256")==context_.S_binding_sha256,"G base policy/schema dependency mismatch");
        const auto* interval=policy.getArray("target_interval_ps");require(interval&&interval->size()==2&&state.period_ps>=*(*interval)[0].getAsNumber()&&state.period_ps<=*(*interval)[1].getAsNumber(),"G shared target outside frozen support");
        auto S=state_.price(p,state,cuts,request_.model_bundle_sha256);require(S.covered,S.reason);require(policy.getString("state_model_sha256")==S.state_model_sha256,"G state parameter identity differs");
        out["state_model_sha256"]=S.state_model_sha256;
        auto diagnostic=llvm::json::parse(S.diagnostics_json);if(!diagnostic)throw std::runtime_error(llvm::toString(diagnostic.takeError()));require(diagnostic->getAsObject(),"missing G actual S diagnostics");const auto& diagnostics=*diagnostic->getAsObject();const auto& state_features=*diagnostics.getObject("owner_features");
        O initial_owners,shared_features;double reset=0.;for(auto owner:{"input_service","native_state","external_state"}){initial_owners[owner]=S.owner_area_um2.at(owner);reset+=number(*state_features.getObject(owner),"reset_one_bits");}
        shared_features["one"]=1.;shared_features["input_data_bits"]=number(*state_features.getObject("input_service"),"data_bits");shared_features["input_ports"]=number(*state_features.getObject("input_service"),"data_signals");shared_features["reset_one_bits"]=reset;
        shared_features["total_FF_bits"]=number(diagnostics,"total_FF_bits");shared_features["sqrt_FF_bits"]=std::sqrt(number(diagnostics,"total_FF_bits"));shared_features["clock_pressure"]=1500./state.period_ps;
        out["owner_area_um2"]=std::move(initial_owners);out["shared_features"]=std::move(shared_features);
        ConditionalOperatorCostProvider operators(p,request_,ConditionalUse::Production);auto rows=operators.placementPrices(p,state.placement,state.internal_stages,cuts.empty()?std::vector<NodeId>{}:cuts.front().cut.controls);
        std::set<NodeId> fused;for(const auto& r:regions)fused.insert(r.additions.begin(),r.additions.end());A raw_nodes;
        for(const auto& row:rows){O n{{"node",row.query.origin}};
            if(!fused.count(row.query.origin))require(row.price.covered,row.price.reason);
            if(row.price.covered){n["delay_ps"]=row.price.operator_delay_ps;n["reserve_ps"]=row.price.delay_reserve_ps;n["area_um2"]=row.price.comb_area_um2;}
            else{n["old_node_price_status"]="not_required_replaced_by_G";n["old_node_reason"]=row.price.reason;}raw_nodes.push_back(std::move(n));}
        out["node_prices"]=std::move(raw_nodes);MutableProgram facts(p);facts.ensureValueFacts();auto normalized_graph=facts.finish();A raw_stages;
        for(std::size_t i=0;i<cuts.size();++i){auto cut=cuts[i];for(const auto& slot:state.slots)if(!slot.valid&&slot.variable_bits){if(slot.stage==cut.cut.index)cut.launches.insert(slot.origin);if(slot.stage==cut.cut.index+1)cut.captures.insert(slot.origin);}
            if(cut.cut.index==0)for(const auto& port:p.ports)if(port.direction==PortDirection::Input)for(auto n:port.element_nodes)if(std::find(cut.cut.controls.begin(),cut.cut.controls.end(),n)==cut.cut.controls.end())cut.launches.insert(n);
            for(const auto& signal:normalized_graph.signals){auto k=kind(signal);if(k=="assign"||k=="cast"||k=="zext"||k=="trunc"||k=="slice"||k=="bitselect"||k=="concat")cut.cut.members.push_back(signal.id);}
            A arcs;for(const auto& a:normalized(normalized_graph,cut,rows))arcs.push_back(arcJson(a,number(policy,"inter_arithmetic_wire_point_ps")));
            raw_stages.push_back(O{{"stage",cut.cut.index},{"cut_sha256",cut.cut.cut_sha256},{"connections",std::move(arcs)},{"nonarithmetic_guard_violation_ps",S.stages.at(i).other_timing_violation_ps}});}
        out["stages"]=std::move(raw_stages);
        std::map<NodeId,double> prices,reserves,areas;std::set<NodeId> hidden;A exports,replaced,nodes;
        double standalone_area=0,regional_area=0;
        for(const auto& r:regions){auto price=regions_.price(r,state.period_ps);auto row=reductionRegionJson(r,state.period_ps);row["delay_ps"]=price.delay_ps;row["area_um2"]=price.area_um2;row["G_model_sha256"]=context_.G_model_sha256;exports.push_back(std::move(row));
            for(auto n:r.additions)require(hidden.insert(n).second,"G region overlaps another region");prices[r.root]=price.delay_ps;reserves[r.root]=0.;areas[r.root]=price.area_um2;regional_area+=price.area_um2;}
        for(const auto& row:*out.getArray("node_prices")){const auto& n=*row.getAsObject();auto id=NodeId(*n.getInteger("node"));
            if(hidden.count(id)){replaced.push_back(O(n));continue;}prices[id]=number(n,"delay_ps");reserves[id]=number(n,"reserve_ps");areas[id]=number(n,"area_um2");standalone_area+=areas.at(id);nodes.push_back(O(n));}
        double margin=number(*policy.getObject("reserve"),"joint_reserve_ps"),wire=number(policy,"inter_arithmetic_wire_point_ps"),violation=0,point_violation=0;
        for(std::size_t i=0;i<cuts.size();++i){auto& stage=*(*out.getArray("stages"))[i].getAsObject();const auto& cut=cuts[i];std::set<NodeId> regional;
            std::map<NodeId,const ReductionRegion*> roots;for(const auto& r:regions)if(r.stage==cut.cut.index){roots[r.root]=&r;regional.insert(r.additions.begin(),r.additions.end());}
            A connections;std::vector<Arc> arcs;
            for(const auto& edge:*stage.getArray("connections")){const auto& e=*edge.getAsObject();Arc a{text(e,"source_kind"),text(e,"sink_kind"),NodeId(*e.getInteger("source")),NodeId(*e.getInteger("sink"))};
                if(a.to_kind=="operator"&&regional.count(a.to))continue;
                require(a.from_kind!="operator"||!regional.count(a.from)||roots.count(a.from),"G interior escapes region");arcs.push_back(a);connections.push_back(O(e));}
            for(const auto& [root,r]:roots)for(std::size_t j=0;j<r->terms.size();++j){std::map<NodeId,A> groups;
                for(auto b:r->terms[j])groups[b.source].push_back(A{b.bit,b.column});
                for(auto& [source,bits]:groups){arcs.push_back({"launch","operator",source,root});connections.push_back(O{{"source_kind","launch"},{"source",source},{"sink_kind","operator"},{"sink",root},{"operand_index",int(j)},
                    {"bit_pairs",std::move(bits)},{"owner","S_boundary"},{"wire_point_ps",0.},{"G_region_input",true}});}}
            std::map<NodeId,std::vector<const Arc*>> preds,captures;for(const auto& a:arcs)(a.to_kind=="operator"?preds:captures)[a.to].push_back(&a);
            std::string capture_owner=cut.cut.last==cut.cut.index?"external_state":"native_state";
            auto boundary=[&](NodeId launch,bool pure){bool native=false;for(const auto& slot:state.slots)if(!slot.external_capture&&!slot.valid&&slot.origin==launch&&slot.stage==cut.cut.index)native=true;
                auto role=(native?"native_state":"input_service")+std::string("->")+capture_owner;
                return number(*policy.getObject(pure?"forward_boundary_point_ps":"boundary_point_ps"),role);};
            std::map<std::pair<NodeId,bool>,double> cache;std::set<NodeId> visiting;
            std::function<double(NodeId,bool)> arrive=[&](NodeId n,bool guard){auto key=std::make_pair(n,guard);if(cache.count(key))return cache.at(key);require(visiting.insert(n).second&&prices.count(n),"G DAG cycle/missing price");double latest=0;
                for(auto a:preds[n])latest=std::max(latest,a->from_kind=="launch"?boundary(a->from,false):arrive(a->from,guard)+wire);
                visiting.erase(n);return cache[key]=latest+prices.at(n)+(guard?reserves.at(n):0.);};
            A endpoints;for(const auto& [capture,edges]:captures){double point=0,guard=0;
                for(auto a:edges){point=std::max(point,a->from_kind=="launch"?boundary(a->from,true):arrive(a->from,false));guard=std::max(guard,a->from_kind=="launch"?boundary(a->from,true):arrive(a->from,true));}
                endpoints.push_back(O{{"node",capture},{"point_required_period_ps",point},{"point_slack_ps",state.period_ps-point},{"legacy_node_guard_required_period_ps",guard},{"decision_required_period_ps",point+margin},{"decision_slack_ps",state.period_ps-point-margin},{"joint_reserve_ps",margin}});
                point_violation=std::max(point_violation,point-state.period_ps);violation=std::max(violation,point+margin-state.period_ps);}
            violation=std::max(violation,number(stage,"nonarithmetic_guard_violation_ps"));stage["connections"]=std::move(connections);stage["captures"]=std::move(endpoints);
        }
        auto& owners=*out.getObject("owner_area_um2");owners["operators"]=standalone_area;owners["G_reductions"]=regional_area;
        double functional=standalone_area+regional_area;for(auto n:{"input_service","native_state","external_state"})functional+=number(owners,n);
        auto& features=*out.getObject("shared_features");features["predicted_functional_area_um2"]=functional;double total=functional;
        for(auto owner:{"shared_clock","shared_physical","root_repair"}){const auto& m=*policy.getObject("shared_area_models")->getObject(owner);const auto& names=*m.getArray("features");const auto& co=*m.getArray("coefficients");double value=0;
            require(names.size()==co.size(),"G shared feature/parameter mismatch");for(std::size_t j=0;j<names.size();++j){auto name=names[j].getAsString()->str();double x=number(features,name);const auto* range=m.getObject("numeric_support")->getArray(name);require(range&&range->size()==2,"missing G shared support");double lo=*(*range)[0].getAsNumber(),hi=*(*range)[1].getAsNumber(),eps=1e-10*std::max({1.,std::abs(lo),std::abs(hi)});
                require(x>=lo-eps&&x<=hi+eps,"G updated shared feature outside frozen support: "+name);value+=x*(*co[j].getAsNumber());}
            require(std::isfinite(value)&&value>=0,"invalid G recomputed shared area");owners[owner]=value;total+=value;}
        out["status"]="candidate_covered";out["version"]="fused-function-composition-v1";out["node_prices"]=std::move(nodes);out["replaced_node_prices"]=std::move(replaced);out["G_regions"]=std::move(exports);
        out["total_area_um2"]=total;out["decision_violation_ps"]=violation;out["point_violation_ps"]=point_violation;
        out["G_delay_ps"]=0.;out["G_area_um2"]=0.;out["G_residual_policy"]="zero residual; whole region replaces its internal Add nodes";
        out["accounting"]="unique S plus standalone nodes plus G regions plus recomputed shared owners";
    }catch(const std::exception& e){out["status"]="cost_uncovered";out["reason"]=std::string(e.what());}
    return out;
}
}
