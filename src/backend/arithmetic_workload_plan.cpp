#include "backend/arithmetic_workload_plan.hpp"
#include "backend/arithmetic_wide_query.hpp"
#include <llvm/ADT/APInt.h>
#include <llvm/ADT/SmallString.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <deque>
#include <functional>
#include <set>
#include <stdexcept>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
void need(bool b,const char* s){if(!b)throw std::runtime_error(s);}
const O& obj(const O& o,const char* k){auto p=o.getObject(k);need(p,"missing workload object");return *p;}
const A& arr(const O& o,const char* k){auto p=o.getArray(k);need(p,"missing workload array");return *p;}
int integer(const O& o,const char* k,int low=0,int high=4096){auto p=o.getInteger(k);need(p&&*p>=low&&*p<=high,"invalid workload integer");return int(*p);}
std::string str(const O& o,const char* k){auto p=o.getString(k);need(bool(p),"missing workload string");return p->str();}
bool bit(const std::vector<std::uint64_t>& words,int b){return b>=0&&std::size_t(b/64)<words.size()&&((words[b/64]>>(b%64))&1);}
struct Atom {
    NodeId source=kInvalidNodeId;int index=0;
    bool operator==(const Atom& b)const{return source==b.source&&index==b.index;}
};
using Bits=std::vector<Atom>;
bool wire(const Operation& d){switch(d.kind){case OperationKind::Assign:case OperationKind::Cast:case OperationKind::ZExt:
case OperationKind::Trunc:case OperationKind::Slice:case OperationKind::BitSelect:case OperationKind::Concat:return true;default:return false;}}
std::string op(const Operation& d){
    if(d.kind==OperationKind::AddCarry)return "adc";
    if(d.kind==OperationKind::Ite)return "mux";
    if(d.kind==OperationKind::Binary)switch(d.op){case OpCode::Add:return "add";case OpCode::Sub:return "sub";
    case OpCode::Mul:return "mul";case OpCode::Lt:return "ult";case OpCode::Le:return "ule";case OpCode::Eq:return "eq";default:break;}
    return "functional";
}
std::string maskHex(const Bits& bits,bool one){llvm::APInt v(std::max<std::size_t>(1,bits.size()),0);
    for(std::size_t b=0;b<bits.size();++b)if(bits[b].source==kInvalidNodeId&&bits[b].index==int(one))v.setBit(b);
    llvm::SmallString<1100> raw;v.toString(raw,16,false);std::string text=raw.str().str();
    std::transform(text.begin(),text.end(),text.begin(),[](unsigned char c){return char(std::tolower(c));});return "0x"+text;}

struct Graph {
    const Program& p;
    std::vector<std::set<int>> demand;
    std::map<NodeId,Bits> cache;
    std::set<NodeId> resolving;
    std::map<NodeId,std::set<NodeId>> root_cache;
    Graph(const Program& graph):p(graph),demand(p.signals.size()){
        std::deque<NodeId> queue;
        auto feed=[&](NodeId n,int lo,int hi){bool changed=false;for(int b=std::max(0,lo);b<std::min(hi,p.signal(n).type.width);++b)changed|=demand[n].insert(b).second;if(changed)queue.push_back(n);};
        for(const auto& port:p.ports)if(port.direction==PortDirection::Output)for(auto n:port.element_nodes)feed(n,0,port.type.width);
        while(!queue.empty()){
            auto i=queue.front();queue.pop_front();const auto& s=p.signal(i);const auto& d=*s.driver;
            need(!s.type.isArray()&&s.type.width>0&&s.type.width<=4096,"workload needs unsigned scalar widths 1..4096");
            if((s.value.valid&&s.value.constant)||d.kind==OperationKind::PortRead)continue;
            if(d.kind==OperationKind::Concat){int base=0;for(auto a=d.operands.rbegin();a!=d.operands.rend();++a){if(a->kind==OperandKind::Symbol)for(int b:demand[i])feed(a->node,b-base,b-base+1);base+=a->type.width;}continue;}
            for(std::size_t k=0;k<d.operands.size();++k){const auto& a=d.operands[k];need(!a.signed_view&&!a.constant.signed_view&&!a.type.isArray(),"unsigned scalar workload operands required");if(a.kind!=OperandKind::Symbol)continue;
                if(d.kind==OperationKind::Slice||d.kind==OperationKind::BitSelect){int lo=d.kind==OperationKind::Slice?d.lo:d.bit;for(int b:demand[i])feed(a.node,lo+b,lo+b+1);}
                else if(wire(d)||(d.kind==OperationKind::Unary&&d.op==OpCode::BitNot)||(d.kind==OperationKind::Ite&&k!=0)){for(int b:demand[i])feed(a.node,b,b+1);}
                else if((d.kind==OperationKind::AddCarry&&k==2)||(d.kind==OperationKind::Ite&&k==0))feed(a.node,0,1);
                else if(op(d)=="add"||op(d)=="adc"||op(d)=="sub"||op(d)=="mul")feed(a.node,0,*demand[i].rbegin()+1);
                else feed(a.node,0,a.type.width);
            }
        }
    }
    Bits operand(const Operand& a){
        if(a.kind==OperandKind::Symbol){auto b=origins(a.node);b.resize(a.type.width);return b;}
        need(a.kind==OperandKind::Literal,"unexpected workload operand kind");Bits out;for(int b=0;b<a.type.width;++b)out.push_back({kInvalidNodeId,int(bit(a.constant.limbs,b))});return out;
    }
    const Bits& origins(NodeId i){
        if(cache.count(i))return cache.at(i);need(resolving.insert(i).second,"cyclic workload graph");
        const auto& s=p.signal(i);const auto& d=*s.driver;Bits out;
        if(d.kind==OperationKind::Concat){for(auto a=d.operands.rbegin();a!=d.operands.rend();++a){auto b=operand(*a);out.insert(out.end(),b.begin(),b.end());}}
        else if(wire(d)){out=operand(d.operands.at(0));int lo=d.kind==OperationKind::Slice?d.lo:d.kind==OperationKind::BitSelect?d.bit:0;
            need(lo>=0&&lo<=int(out.size()),"workload wire slice");out.erase(out.begin(),out.begin()+lo);out.resize(s.type.width);}
        else if(d.kind==OperationKind::Binary&&(d.op==OpCode::BitAnd||d.op==OpCode::BitOr||d.op==OpCode::BitXor)){
            auto a=operand(d.operands[0]),b=operand(d.operands[1]);a.resize(s.type.width);b.resize(s.type.width);
            Atom zero{},one{kInvalidNodeId,1};
            for(int j=0;j<s.type.width;++j){auto x=a[j],y=b[j];Atom v{i,j};
                if(x==y)v=d.op==OpCode::BitXor?zero:x;
                else if(d.op==OpCode::BitAnd){if(x==zero||y==zero)v=zero;else if(x==one)v=y;else if(y==one)v=x;}
                else if(d.op==OpCode::BitOr){if(x==one||y==one)v=one;else if(x==zero)v=y;else if(y==zero)v=x;}
                else {if(x==zero)v=y;else if(y==zero)v=x;}out.push_back(v);
            }
        }else for(int b=0;b<s.type.width;++b)out.push_back({i,b});
        need(out.size()==std::size_t(s.type.width),"workload bit mapping width");
        for(int b=0;b<s.type.width;++b)if(s.value.valid&&(bit(s.value.known_zero,b)||bit(s.value.known_one,b)))out[b]={kInvalidNodeId,int(bit(s.value.known_one,b))};
        resolving.erase(i);return cache.emplace(i,std::move(out)).first->second;
    }
    bool alias(NodeId n){const auto& b=origins(n);return std::none_of(b.begin(),b.end(),[&](auto b){return b.source==n;});}
    const std::set<NodeId>& roots(NodeId n){
        if(root_cache.count(n))return root_cache.at(n);std::set<NodeId> out;const auto& s=p.signal(n);
        if(!(s.value.valid&&s.value.constant)){if(s.driver->kind==OperationKind::PortRead)out.insert(n);
            else for(const auto& a:s.driver->operands)if(a.kind==OperandKind::Symbol){const auto& sub=roots(a.node);out.insert(sub.begin(),sub.end());}}
        return root_cache.emplace(n,std::move(out)).first->second;
    }
    int billing(Operand a){while(a.kind==OperandKind::Symbol){const auto& s=p.signal(a.node);const auto& d=*s.driver;
        if((d.kind!=OperationKind::Assign&&d.kind!=OperationKind::Cast&&d.kind!=OperationKind::ZExt&&d.kind!=OperationKind::Trunc)||d.operands.size()!=1||d.operands[0].type.width>a.type.width)break;a=d.operands[0];}return a.type.width;}
    O spec(NodeId n){const auto& d=*p.signal(n).driver;auto name=op(d);need(name!="functional","not a workload arithmetic owner");
        // BEIR Ite is [selector, true, false]; the shared wide contract is
        // cin ? rhs : lhs. Preserve this orientation even for unequal masks.
        const auto& a=d.operands[name=="mux"?2:0];const auto& b=d.operands[name=="mux"?1:1];int wa=billing(a),wb=billing(b),r=*demand[n].rbegin()+1;
        int w=(name=="ult"||name=="ule"||name=="eq")?std::max(wa,wb):name=="mul"?std::max(r,wa+wb):std::max({r,wa,wb});
        std::string carry="zero";if(name=="adc"||name=="mux"){auto c=operand(d.operands[name=="mux"?0:2]).at(0);carry=c.source!=kInvalidNodeId?"dynamic":c.index?"one":"zero";}
        if(name=="adc"&&carry=="zero")name="add";
        auto ab=operand(a),bb=operand(b);ab.resize(wa);bb.resize(wb);
        return O{{"operation",name},{"lhs_width",wa},{"rhs_width",wb},{"arithmetic_width",w},{"result_lo",0},{"value_width",r},{"status","none"},{"result_width",r},
            {"carry",carry},{"capture_banks",1},{"lhs_zero_hex",maskHex(ab,false)},{"lhs_one_hex",maskHex(ab,true)},{"rhs_zero_hex",maskHex(bb,false)},{"rhs_one_hex",maskHex(bb,true)}};
    }
};
A bitMap(const Bits& bits,NodeId producer,int count,const std::set<int>* demand=nullptr){A out;
    for(int i=0;i<count;++i){Atom b=i<int(bits.size())?bits[i]:Atom{};
        if(demand&&!demand->count(i))out.push_back("undemanded");else if(b.source==producer)out.push_back(b.index);
        else if(b.source==kInvalidNodeId)out.push_back(b.index?"1":"0");else out.push_back("external");}return out;}
bool mapped(const A& a){return std::any_of(a.begin(),a.end(),[](const V& v){return bool(v.getAsInteger());});}
}

O projectWorkloadQuery(const O& q){
    need(q.getString("version")=="arithmetic-workload-query-v2","unknown workload query version");
    O out(q);out["version"]="arithmetic-wide-query-v1";auto& local=*out.getObject("local_context");
    for(auto& raw:*local.getArray("consumers")){auto& c=*raw.getAsObject();
        for(auto& rb:*c.getArray("bindings")){auto& b=*rb.getAsObject();need(b.getString("role")=="data","v1 cannot represent carry/selector observation");b.erase("role");}
        c.erase("operation");
    }
    // The validator rejects all other v1 limitations; never delete consumers,
    // captures or mappings to manufacture an accepted projection.
    wideOperatorQueryFeatures(out);return out;
}

std::map<std::string,double> workloadQueryFeatures(const O& q){
    need(q.getString("version")=="arithmetic-workload-query-v2","unknown workload query");
    const auto& local=obj(q,"local_context");const auto& p=obj(local,"producer");
    evaluateWideOperator(p,"0","0",false); // authoritative scalar semantic validator
    auto t=obj(q,"flow_context").getNumber("target_period_ps");need(t&&std::isfinite(*t)&&*t>0,"invalid workload T");
    std::map<std::string,double> f{{"lhs_width",integer(p,"lhs_width",1)},{"rhs_width",integer(p,"rhs_width",1)},
        {"arithmetic_width",integer(p,"arithmetic_width",1)},{"result_width",integer(p,"result_width",1)},
        {"target_ps",*t},{"clock_pressure",1500. / *t},{"consumer_count",arr(local,"consumers").size()},
        {"capture_count",arr(local,"captures").size()},{"data_pin_uses",0},{"carry_pin_uses",0},{"selector_pin_uses",0},{"capture_pin_uses",0}};
    int r=integer(p,"result_width",1);auto map=[&](const A& bits,const std::string& key){for(const auto& b:bits){if(auto n=b.getAsInteger()){need(*n>=0&&*n<r,"workload mapped producer bit outside demand");++f.at(key);}
        else{auto s=b.getAsString();need(s&&(*s=="0"||*s=="1"||*s=="external"||*s=="undemanded"),"invalid workload map token");}}};
    for(const auto& raw:arr(local,"consumers")){auto c=raw.getAsObject();need(c,"invalid workload consumer");
        for(const auto& rb:arr(*c,"bindings")){auto b=rb.getAsObject();need(b,"invalid workload binding");auto role=str(*b,"role");need(role=="data"||role=="carry"||role=="selector","invalid workload pin role");map(arr(*b,"result_bit_map"),role+"_pin_uses");}}
    for(const auto& raw:arr(local,"captures")){auto c=raw.getAsObject();need(c,"invalid workload capture");map(arr(*c,"result_bit_map"),"capture_pin_uses");}
    need(f["data_pin_uses"]+f["carry_pin_uses"]+f["selector_pin_uses"]+f["capture_pin_uses"]>0,"unobserved workload owner");return f;
}

WorkloadPlan workloadPlan(const pipeline::Program& actual,double target,const std::string& profile){
    need(std::isfinite(target)&&target>0&&!profile.empty(),"workload plan flow required");
    const auto& p=actual.combinational;Graph g(p);auto state=stateCostQuery(actual,target,true);const auto& stages=actual.stages;
    WorkloadPlan plan;plan.target_ps=target;plan.graph_sha256=state.graph_sha256;plan.layout_sha256=state.layout_sha256;plan.profile_sha256=profile;
    std::map<NodeId,O> specs;
    for(const auto& s:p.signals)if(!g.demand[s.id].empty()&&!(s.value.valid&&s.value.constant)&&s.driver->kind!=OperationKind::PortRead&&!g.alias(s.id)){
        bool dynamic_demand=false;for(int b:g.demand[s.id])dynamic_demand|=g.origins(s.id)[b].source==s.id;
        if(!dynamic_demand)continue;
        if(op(*s.driver)!="functional")specs.emplace(s.id,g.spec(s.id));else if(!wire(*s.driver))plan.functional_logic.push_back(s.id);
    }
    struct Connection{NodeId sink;int pin;std::string role;A mapping;};
    std::map<NodeId,std::vector<Connection>> connections;
    std::map<NodeId,std::set<NodeId>> users;
    for(const auto& s:p.signals)if(!g.demand[s.id].empty()&&!(s.value.valid&&s.value.constant)&&s.driver->kind!=OperationKind::PortRead&&!g.alias(s.id)){
        const auto& d=*s.driver;for(std::size_t k=0;k<d.operands.size();++k){auto bits=g.operand(d.operands[k]);std::set<NodeId> sources;
            for(auto b:bits)if(b.source!=kInvalidNodeId&&b.source!=s.id)sources.insert(b.source);
            for(auto n:sources){users[n].insert(s.id);if(!specs.count(n))continue;
                auto name=op(d);std::string role=name=="adc"&&k==2?"carry":name=="mux"&&k==0?"selector":"data";
                connections[n].push_back({s.id,int(k),role,bitMap(bits,n,bits.size())});}}
    }
    std::set<NodeId> observed;
    std::map<std::tuple<NodeId,int,int>,std::pair<NodeId,int>> saved;
    std::map<std::string,NodeId> native_names;for(const auto& s:actual.rtl_program.signals)native_names[s.name]=s.id;
    for(const auto& slot:state.slots)if(!slot.valid&&!slot.external_capture&&slot.variable_bits){const auto& bits=g.origins(slot.origin);
        need(native_names.count(slot.name),"verified state slot lacks materialized signal");
        for(int b=0;b<int(bits.size());++b)if(bits[b].source!=kInvalidNodeId){auto key=std::make_tuple(bits[b].source,bits[b].index,slot.stage);auto value=std::make_pair(native_names.at(slot.name),b);
            auto found=saved.find(key);if(found==saved.end()||value<found->second)saved[key]=value;}}
    for(const auto& slot:state.slots)if(!slot.valid&&slot.variable_bits)for(auto b:g.origins(slot.origin))
        if(b.source!=kInvalidNodeId&&slot.stage==stages[b.source]+1)observed.insert(b.source);
    // Downstream before upstream, using dependencies rather than numeric IDs.
    std::vector<NodeId> order;std::set<NodeId> visited;
    std::function<void(NodeId)> visit=[&](NodeId n){if(!visited.insert(n).second)return;for(const auto& a:p.signal(n).driver->operands)if(a.kind==OperandKind::Symbol)visit(a.node);order.push_back(n);};
    for(const auto& [n,s]:specs)visit(n);
    auto add=[&](NodeId n){return specs.count(n)&&(specs.at(n).getString("operation")=="add");};
    for(auto it=order.rbegin();it!=order.rend();++it){NodeId root=*it;if(!add(root)||plan.reduction_owners.count(root))continue;
        ReductionRegion region;region.root=root;region.stage=stages[root];region.width=integer(specs.at(root),"result_width",1);
        std::set<NodeId> members{root};bool safe=true;
        std::function<void(NodeId,int)> flatten=[&](NodeId n,int offset){const auto& d=*p.signal(n).driver;
            for(const auto& a:std::vector<Operand>(d.operands.begin(),d.operands.begin()+2)){auto bits=g.operand(a);std::map<NodeId,std::vector<std::pair<int,int>>> groups;std::vector<ReductionBit> leaves;
                for(int col=0;col<int(bits.size())&&col+offset<region.width;++col){auto b=bits[col];if(b.source==kInvalidNodeId){safe&=b.index==0;continue;}groups[b.source].push_back({b.index,col+offset});}
                for(const auto& [child,pairs]:groups){int shift=pairs.front().second-pairs.front().first;bool flat=add(child)&&!plan.reduction_owners.count(child)&&!observed.count(child)&&stages[child]==region.stage&&users[child].size()==1&&shift>=0;
                    std::set<std::pair<int,int>> expected,got(pairs.begin(),pairs.end());const auto& origin=g.origins(child);
                    for(int b=0;b<int(origin.size())&&b+shift<region.width;++b)if(origin[b].source!=kInvalidNodeId||origin[b].index)expected.insert({b,b+shift});
                    flat&=got==expected;
                    if(flat&&p.signal(child).type.width+shift<region.width){llvm::APInt maximum(8193,0);for(const auto& operand:p.signal(child).driver->operands){auto ob=g.operand(operand);for(int b=0;b<int(ob.size());++b)if(ob[b].source!=kInvalidNodeId||ob[b].index)maximum+=llvm::APInt::getOneBitSet(8193,b);}flat=maximum.getActiveBits()<=unsigned(p.signal(child).type.width);}
                    if(flat){safe&=members.insert(child).second;flatten(child,shift);}else for(auto [b,c]:pairs)leaves.push_back({child,b,c});}
                if(!leaves.empty())region.terms.push_back(std::move(leaves));
            }};
        flatten(root,0);if(!safe||members.size()<2||region.terms.size()<3)continue;
        // Repeated atoms are outside this emitter contract. Distinct sources
        // with shared input roots are retained in the plan but refused prices.
        try{O q;A terms;for(const auto& t:region.terms){A bits;for(auto b:t)bits.push_back(A{b.source,b.bit,b.column});terms.push_back(std::move(bits));}
            q["output_bits"]=region.width;q["terms"]=std::move(terms);q["target_ps"]=target;
            // Existing legacy reductionFeatures is intentionally not called:
            // its 257-bit gate is a different measurement contract.
            std::set<std::pair<NodeId,int>> unique;for(const auto& t:region.terms)for(auto b:t)need(unique.insert({b.source,b.bit}).second,"repeated G atom requires correlation qualification");
        }catch(const std::runtime_error&){continue;}
        bool launched=true;std::map<std::tuple<NodeId,int,int>,std::pair<NodeId,int>> leaves;
        for(const auto& term:region.terms)for(auto b:term){auto key=std::make_tuple(b.source,b.bit,region.stage);
            if(stages[b.source]==region.stage)leaves[key]={b.source,b.bit};
            else if(saved.count(key))leaves[key]=saved.at(key);else launched=false;}
        if(!launched)continue;
        plan.launches.insert(leaves.begin(),leaves.end());
        region.additions.assign(members.begin(),members.end());region.members=region.additions;
        for(auto n:members)plan.reduction_owners.emplace(n,root);plan.reductions.push_back(std::move(region));
    }
    for(const auto& region:plan.reductions){A terms;bool registered=true;std::set<NodeId> sinks,sources;
        for(const auto& term:region.terms){A bits;for(auto b:term){bits.push_back(A{b.source,b.bit,b.column});
                sources.insert(b.source);
                registered&=stages[b.source]<region.stage||p.signal(b.source).driver->kind==OperationKind::PortRead;}terms.push_back(std::move(bits));}
        int shared=0;for(auto a=sources.begin();a!=sources.end();++a)for(auto b=std::next(a);b!=sources.end();++b){
            const auto& ar=g.roots(*a);const auto& br=g.roots(*b);std::vector<NodeId> common;
            std::set_intersection(ar.begin(),ar.end(),br.begin(),br.end(),std::back_inserter(common));shared+=!common.empty();}
        for(auto n:users[region.root])if(stages[n]==region.stage)sinks.insert(plan.reduction_owners.count(n)?plan.reduction_owners.at(n):n);
        int captures=0;for(const auto& slot:state.slots)if(!slot.valid&&slot.variable_bits&&slot.stage==region.stage+1){auto mapping=bitMap(g.origins(slot.origin),region.root,slot.declared_width,&g.demand[slot.origin]);captures+=mapped(mapping);}
        plan.reduction_queries.emplace(region.root,O{{"output_bits",region.width},{"terms",std::move(terms)},{"target_ps",target},
            {"launch_type",registered?"register":"combinational"},{"capture_banks",captures},{"consumer_count",sinks.size()},
            {"shared_input_root_pairs",shared}});
    }
    for(auto& [n,spec]:specs){if(plan.reduction_owners.count(n))continue;
        A consumers,captures;std::map<NodeId,A> groups;
        for(const auto& c:connections[n])if(stages[c.sink]==stages[n]&&!plan.reduction_owners.count(c.sink)){
            const auto& d=*p.signal(c.sink).driver;int pin=c.pin;
            if(op(d)=="mux"&&c.role=="data")pin=c.pin==2?0:1;
            int limit=c.role=="data"&&specs.count(c.sink)?integer(specs.at(c.sink),pin==0?"lhs_width":"rhs_width",1):int(c.mapping.size());A mapping;
            for(int b=0;b<limit;++b)mapping.push_back(b<int(c.mapping.size())?V(c.mapping[b]):V("0"));
            if(specs.count(c.sink)&&c.role=="data"&&op(d)!="mux"&&op(d)!="ult"&&op(d)!="ule"&&op(d)!="eq"){
                int r=integer(specs.at(c.sink),"result_width",1);for(int b=r;b<int(mapping.size());++b)mapping[b]="undemanded";}
            // Never charge a bit outside the producer demand: any remaining
            // value at that pin is a proven dead result position of the sink.
            int r=integer(spec,"result_width",1);for(auto& b:mapping)if(auto k=b.getAsInteger();k&&*k>=r)b="undemanded";
            if(mapped(mapping))groups[c.sink].push_back(O{{"operand_index",pin},{"role",c.role},{"result_bit_map",std::move(mapping)}});
        }
        for(const auto& region:plan.reductions)if(region.stage==stages[n]){std::set<int> used;for(const auto& term:region.terms)for(auto b:term)if(b.source==n)used.insert(b.bit);
            if(!used.empty()){A mapping;for(int b=0;b<p.signal(n).type.width;++b)mapping.push_back(used.count(b)?V(b):V("undemanded"));
                groups[region.root].push_back(O{{"operand_index",0},{"role","data"},{"result_bit_map",std::move(mapping)}});}}
        for(auto& [sink,bindings]:groups){bool reduction=plan.reduction_owners.count(sink);O consumer{{"operation",reduction?"reduction":op(*p.signal(sink).driver)},{"bindings",std::move(bindings)},
                {"other_dynamic_inputs","independent_launch; source correlations require G validation"}};
            if(specs.count(sink)&&!reduction)consumer["spec"]=O(specs.at(sink));else consumer["spec"]=nullptr;
            consumers.push_back(std::move(consumer));}
        for(const auto& slot:state.slots)if(!slot.valid&&slot.variable_bits&&slot.stage==stages[n]+1){auto mapping=bitMap(g.origins(slot.origin),n,slot.declared_width,&g.demand[slot.origin]);if(mapped(mapping))captures.push_back(O{{"banks",1},{"declared_width",slot.declared_width},{"result_bit_map",std::move(mapping)}});}
        const auto& d=*p.signal(n).driver;const auto& a=d.operands[op(d)=="mux"?2:0];const auto& b=d.operands[1];std::set<NodeId> roots_a,roots_b,common;
        if(a.kind==OperandKind::Symbol)roots_a=g.roots(a.node);if(b.kind==OperandKind::Symbol)roots_b=g.roots(b.node);
        std::set_intersection(roots_a.begin(),roots_a.end(),roots_b.begin(),roots_b.end(),std::inserter(common,common.begin()));
        std::map<NodeId,int> depth_cache;std::function<int(NodeId)> depth=[&](NodeId at){if(depth_cache.count(at))return depth_cache.at(at);int max=0;for(auto u:users[at])if(stages[u]==stages[at])max=std::max(max,depth(u));return depth_cache[at]=int(specs.count(at))+max;};
        O q{{"version","arithmetic-workload-query-v2"},{"semantics","unsigned-wide-arithmetic-semantics-v1"},{"declared_result_width",p.signal(n).type.width},
            {"local_context",O{{"producer",O(spec)},{"consumers",std::move(consumers)},{"captures",std::move(captures)}}},
            {"flow_context",O{{"target_platform","asap7"},{"evaluation_stage","grt"},{"target_period_ps",target},{"abc_target_ps",target},{"profile_sha256",profile},{"boundary_policy","unsigned-wide-family-hierarchy-v1"}}},
            {"topology",O{{"operand_shared_input_roots",common.size()},{"same_stage_remaining_depth",depth(n)}}}};
        A required;for(int b=0;b<integer(spec,"result_width",1);++b)required.push_back(b);q["required_result_bits"]=std::move(required);
        WorkloadOperator row;row.node=n;row.stage=stages[n];row.query=std::move(q);
        try{row.predecessor_query=projectWorkloadQuery(row.query);}catch(const std::exception& e){row.predecessor_rejection=e.what();}
        // Interface validity is checked independently of v1 projection support.
        workloadQueryFeatures(row.query);plan.operators.push_back(std::move(row));
    }
    return plan;
}

O workloadPlanJson(const WorkloadPlan& p){A operators,regions,logic,launches;
    for(const auto& r:p.operators){O features;for(const auto& [n,v]:workloadQueryFeatures(r.query))features[n]=v;
        O row{{"node",r.node},{"stage",r.stage},{"query",O(r.query)},{"interface_features",std::move(features)},{"predecessor_projection",r.predecessor_query?V(O(*r.predecessor_query)):V(nullptr)},
            {"predecessor_rejection",r.predecessor_rejection}};operators.push_back(std::move(row));}
    for(const auto& r:p.reductions){A members,terms;for(auto n:r.members)members.push_back(n);for(const auto& t:r.terms){A bits;for(auto b:t)bits.push_back(A{b.source,b.bit,b.column});terms.push_back(std::move(bits));}
        regions.push_back(O{{"root",r.root},{"stage",r.stage},{"output_bits",r.width},{"members",std::move(members)},{"terms",std::move(terms)},
            {"query",O(p.reduction_queries.at(r.root))},{"implementation","kept sum expression; same source form as G sampling"},{"price_qualification_granted",false}});}
    for(auto n:p.functional_logic)logic.push_back(n);
    for(const auto& [key,value]:p.launches){auto [source,bit,stage]=key;launches.push_back(O{{"source",source},{"bit",bit},{"stage",stage},{"materialized_node",value.first},{"materialized_bit",value.second}});}
    return O{{"version","workload-owner-plan-v2"},{"boundary_policy",kWorkloadBoundaryPolicy},{"graph_sha256",p.graph_sha256},{"layout_sha256",p.layout_sha256},{"profile_sha256",p.profile_sha256},
        {"target_ps",p.target_ps},{"operators",std::move(operators)},{"G_regions",std::move(regions)},{"functional_logic",std::move(logic)},{"launch_bits",std::move(launches)},
        {"unique_arithmetic_ownership_checked",true},{"model_boundary_physical_qualification",false},{"full_D_ready",false}};
}
}
