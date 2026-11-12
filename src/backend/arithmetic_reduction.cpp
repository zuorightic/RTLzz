#include "backend/arithmetic_reduction.hpp"
#include "backend/arithmetic_condition_selection.hpp"
#include <llvm/ADT/APInt.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>

namespace pred::beir::arithmetic {
namespace {
void require(bool x,const char* why){if(!x)throw std::runtime_error(why);}
bool bit(const std::vector<std::uint64_t>& v,int b){return std::size_t(b/64)<v.size()&&((v[b/64]>>(b%64))&1);}
bool add(const Signal& s){return s.driver&&(s.driver->kind==OperationKind::AddCarry||
    (s.driver->kind==OperationKind::Binary&&s.driver->op==OpCode::Add));}
bool pure(const Signal& s){if(!s.driver)return false;switch(s.driver->kind){
    case OperationKind::Assign:case OperationKind::Cast:case OperationKind::ZExt:
    case OperationKind::Trunc:case OperationKind::Slice:case OperationKind::BitSelect:
    case OperationKind::Concat:return true;default:return false;}}
struct Atom {NodeId node=kInvalidNodeId;int bit=0;bool arithmetic=false;int constant=0;};
}

std::map<std::string,double> reductionFeatures(int width,const std::vector<std::vector<ReductionBit>>& terms,double target){
    require(width>0&&width<=257&&std::isfinite(target)&&target>0,"invalid reduction dimensions/period");
    std::vector<int> heights(width);int total=0,maxheight=0,low=width,high=-1;
    std::set<std::pair<NodeId,int>> independent;
    for(const auto& term:terms){require(!term.empty(),"empty reduction term");std::set<int> columns;for(const auto& b:term){
        require(b.source!=kInvalidNodeId&&b.bit>=0&&b.column>=0&&b.column<width&&columns.insert(b.column).second,"invalid reduction term column");
        require(independent.insert({b.source,b.bit}).second,"correlated/repeated reduction input bit");
        ++heights[b.column];++total;low=std::min(low,b.column);high=std::max(high,b.column);}}
    require(terms.size()>=3&&total>0,"reduction requires at least three live terms");
    for(int h:heights)maxheight=std::max(maxheight,h);
    int first=width;for(int i=0;i<width;++i)if(heights[i]>=2){first=i;break;}
    int counters=0,depth=0;
    for(;;){bool needed=false;for(int h:heights)needed|=h>2;if(!needed)break;
        std::vector<int> next(width);for(int b=0;b<width;++b){int n=heights[b]/3;counters+=n;next[b]+=heights[b]%3+n;if(b+1<width)next[b+1]+=n;}
        heights=std::move(next);require(++depth<=257,"unbounded compressor analysis");}
    double carry=std::max(0,high-first+1);
    int rows=maxheight,levels=0;while(rows>2){rows=2*(rows/3)+rows%3;++levels;}
    return {{"one",1.},{"output_bits",double(width)},{"term_count",double(terms.size())},
        {"active_bits",double(total)},{"max_column_height",double(maxheight)},
        {"counter_count",double(counters)},{"compressor_depth",double(depth)},{"compression_levels",double(levels)},
        {"carry_span",carry},{"log2_carry_span",std::log2(carry+1.)},
        {"active_span",double(high-low+1)},{"clock_pressure",1500./target},
        {"log2_target",std::log2(target/500.)},
        {"pressure_work",(counters+carry)*1500./target}};
}

std::vector<ReductionRegion> reductionRegions(const Program& original,const std::vector<int>& placement,int last,const std::vector<NodeId>& controls){
    MutableProgram facts(original);facts.ensureValueFacts();auto p=facts.finish();
    require(placement.size()==p.signals.size(),"reduction placement shape mismatch");
    auto cuts=conditionalBoundaryRequests(p,placement,last,controls);
    auto queries=operatorQueries(p,placement,last,1500.,"discovery-only", "unsigned-op-hierarchy-v1",controls);
    std::map<NodeId,int> demanded,capture_banks,consumers;
    for(const auto& q:queries){demanded[q.origin]=int(*q.descriptor.getObject("local_context")->getObject("producer")->getInteger("result_width"));capture_banks[q.origin]=int(q.features.at("capture_banks"));consumers[q.origin]=int(q.features.at("consumer_count"));}
    std::vector<std::set<NodeId>> users(p.signals.size());std::set<NodeId> outputs;
    for(const auto& s:p.signals)if(s.driver)for(const auto& o:s.driver->operands)if(o.kind==OperandKind::Symbol)users[o.node].insert(s.id);
    for(const auto& port:p.ports)if(port.direction==PortDirection::Output)outputs.insert(port.element_nodes.begin(),port.element_nodes.end());
    std::vector<ReductionRegion> result;std::set<NodeId> claimed;
    // Distinct launch symbols can denote the same delayed pure-wire bit. Use
    // their semantic bit origin to reject such aliases before pricing a region.
    std::function<std::pair<NodeId,int>(NodeId,int)> alias=[&](NodeId n,int b)->std::pair<NodeId,int>{
        const auto& s=p.signal(n);if(!pure(s))return {n,b};const auto& d=*s.driver;
        const Operand* chosen=nullptr;int index=b;
        if(d.kind==OperationKind::Concat){for(std::size_t j=d.operands.size();j-->0;){if(index<d.operands[j].type.width){chosen=&d.operands[j];break;}index-=d.operands[j].type.width;}}
        else {require(d.operands.size()==1,"alias wire arity");chosen=&d.operands[0];if(d.kind==OperationKind::Slice)index+=d.lo;else if(d.kind==OperationKind::BitSelect)index+=d.bit;}
        require(chosen&&index>=0&&index<chosen->type.width&&chosen->kind==OperandKind::Symbol,"variable launch bit has constant alias");return alias(chosen->node,index);
    };
    for(const auto& cut:cuts){
        auto launch=cut.launches;for(const auto& port:p.ports)if(port.direction==PortDirection::Input)launch.insert(port.element_nodes.begin(),port.element_nodes.end());
        std::map<NodeId,std::vector<Atom>> cache;std::set<NodeId> resolving;
        std::function<std::vector<Atom>(NodeId)> resolve;
        std::function<std::vector<Atom>(const Operand&)> operand=[&](const Operand& o){
            require(!o.signed_view&&!o.constant.signed_view&&!o.type.isArray(),"signed/array reduction operand");
            if(o.kind==OperandKind::Symbol)return resolve(o.node);
            require(o.kind==OperandKind::Literal,"unknown reduction operand");std::vector<Atom> v;
            for(int b=0;b<o.type.width;++b)v.push_back({kInvalidNodeId,0,false,bit(o.constant.limbs,b)?1:0});return v;};
        resolve=[&](NodeId n)->std::vector<Atom>{
            if(cache.count(n))return cache.at(n);require(resolving.insert(n).second,"cyclic reduction wire");
            const auto& s=p.signal(n);require(!s.type.isArray()&&s.type.width>0&&s.type.width<=257,"reduction scalar width");std::vector<Atom> v;
            if(s.value.valid&&s.value.constant){for(int b=0;b<s.type.width;++b)v.push_back({kInvalidNodeId,0,false,bit(s.value.known_one,b)?1:0});}
            else if(launch.count(n)||placement[n]<cut.cut.index||add(s)){
                bool a=!launch.count(n)&&placement[n]==cut.cut.index&&add(s);for(int b=0;b<s.type.width;++b)v.push_back({n,b,a,0});
            }else{
                require(pure(s)&&s.driver&&!s.driver->signed_truncation,"non-register-launched reduction input");const auto& d=*s.driver;
                if(d.kind==OperationKind::Concat){for(std::size_t i=d.operands.size();i-->0;){auto sub=operand(d.operands[i]);v.insert(v.end(),sub.begin(),sub.end());}}
                else{require(d.operands.size()==1,"reduction wire arity");v=operand(d.operands[0]);
                    if(d.kind==OperationKind::Slice||d.kind==OperationKind::BitSelect){int lo=d.kind==OperationKind::Slice?d.lo:d.bit;require(lo>=0&&lo+s.type.width<=int(v.size()),"reduction slice");v=std::vector<Atom>(v.begin()+lo,v.begin()+lo+s.type.width);}
                    else v.resize(s.type.width);}
            }
            require(v.size()==std::size_t(s.type.width),"reduction wire width mismatch");
            for(int b=0;b<s.type.width;++b)if(s.value.valid&&(bit(s.value.known_zero,b)||bit(s.value.known_one,b)))v[b]={kInvalidNodeId,0,false,bit(s.value.known_one,b)?1:0};
            resolving.erase(n);cache[n]=v;return v;
        };
        // Roots with another arithmetic consumer are not region outputs.
        for(auto it=p.signals.rbegin();it!=p.signals.rend();++it){const auto& root=*it;if(!add(root)||placement[root.id]!=cut.cut.index||claimed.count(root.id)||!demanded.count(root.id))continue;
            try{
                require(capture_banks.at(root.id)==1&&consumers.at(root.id)==0,"reduction output requires one actual capture bank and no arithmetic consumer");
                ReductionRegion region;region.root=root.id;region.stage=cut.cut.index;region.width=demanded.at(root.id);
                std::set<NodeId> adds;std::set<std::pair<NodeId,int>> independent;
                std::function<void(NodeId,int,int)> flatten=[&](NodeId n,int offset,int count){
                    require(adds.insert(n).second&&!claimed.count(n),"shared reduction arithmetic interior");
                    require(placement[n]==cut.cut.index&&!launch.count(n),"reduction crosses register");
                    const auto& s=p.signal(n);require(add(s)&&s.driver&&!s.driver->signed_truncation,"unsupported reduction arithmetic");
                    auto start=region.terms.size();
                    for(const auto& o:s.driver->operands){auto bits=operand(o);std::map<NodeId,std::vector<std::pair<int,int>>> children;std::vector<ReductionBit> term;
                        for(int b=0;b<std::min(count,int(bits.size()));++b){const auto& a=bits[b];
                            if(a.node==kInvalidNodeId){require(a.constant==0,"nonzero reduction constant outside first-version domain");continue;}
                            if(a.arithmetic)children[a.node].push_back({a.bit,b});
                            else {require(independent.insert(alias(a.node,a.bit)).second,"correlated/repeated reduction input bit");term.push_back({a.node,a.bit,offset+b});}}
                        if(!term.empty())region.terms.push_back(std::move(term));
                        for(const auto& [child,mapping]:children){require(!mapping.empty(),"empty child bit map");int shift=mapping.front().second;
                            for(std::size_t j=0;j<mapping.size();++j)require(mapping[j].first==int(j)&&mapping[j].second==shift+int(j),"non-low or permuted intermediate sum");
                            flatten(child,offset+shift,int(mapping.size()));}
                    }
                    // A discarded carry may not reappear through zero extension.
                    // Modulo is safe only at the final demanded high boundary,
                    // or if a worst-case independent-bit sum proves no overflow.
                    if(offset+count<region.width){llvm::APInt maximum(512,0);
                        for(std::size_t j=start;j<region.terms.size();++j){llvm::APInt term(512,0);for(auto b:region.terms[j])term.setBit(b.column-offset);maximum+=term;}
                        require(maximum.getActiveBits()<=unsigned(count),"intermediate modulo followed by widening");}
                };
                flatten(root.id,0,region.width);require(adds.size()>=2&&region.terms.size()>=3,"not a multi-node reduction");
                std::set<NodeId> members=adds;
                std::function<bool(NodeId)> interior=[&](NodeId n){if(adds.count(n))return true;if(launch.count(n)||placement[n]!=cut.cut.index||!pure(p.signal(n)))return false;
                    bool contains=false;for(const auto& o:p.signal(n).driver->operands)if(o.kind==OperandKind::Symbol)contains|=interior(o.node);
                    if(contains)members.insert(n);return contains;};interior(root.id);
                // Walk the original operands to include pure wires between Adds.
                for(auto n:adds)for(const auto& o:p.signal(n).driver->operands)if(o.kind==OperandKind::Symbol)interior(o.node);
                for(auto n:members)if(n!=root.id){require(!outputs.count(n),"observed reduction interior");for(auto u:users[n])require(members.count(u),"shared/registered reduction interior");}
                // Root may feed only pure wiring ending at capture, never another
                // arithmetic node or repeated/multi-bank load in this version.
                std::set<NodeId> seen;int captures=0;std::function<void(NodeId)> sinks=[&](NodeId n){if(!seen.insert(n).second)return;
                    if(cut.captures.count(n)){++captures;return;}require(!outputs.count(n),"missing reduction capture contract");
                    for(auto u:users[n]){require(placement[u]==cut.cut.index&&pure(p.signal(u)),"reduction root has arithmetic or cross-stage consumer");sinks(u);}};sinks(root.id);
                require(captures==1,"reduction root requires exactly one capture bank");
                region.additions.assign(adds.begin(),adds.end());region.members.assign(members.begin(),members.end());region.features=reductionFeatures(region.width,region.terms,1500.);
                claimed.insert(adds.begin(),adds.end());result.push_back(std::move(region));
            }catch(const std::runtime_error&){resolving.clear(); /* deterministic original-boundary fallback */}
        }
    }
    std::sort(result.begin(),result.end(),[](const auto& a,const auto& b){return std::tie(a.stage,a.root)<std::tie(b.stage,b.root);});return result;
}

llvm::json::Object reductionRegionJson(const ReductionRegion& r,double t){
    llvm::json::Array adds,members,terms;for(auto n:r.additions)adds.push_back(n);for(auto n:r.members)members.push_back(n);
    for(const auto& term:r.terms){llvm::json::Array bits;for(auto b:term)bits.push_back(llvm::json::Array{b.source,b.bit,b.column});terms.push_back(std::move(bits));}
    llvm::json::Object f;for(auto [n,v]:reductionFeatures(r.width,r.terms,t))f[n]=v;
    return llvm::json::Object{{"policy",kReductionPolicy},{"root",r.root},{"stage",r.stage},{"output_bits",r.width},
        {"additions",std::move(adds)},{"members",std::move(members)},{"terms",std::move(terms)},{"features",std::move(f)},
        {"load","single_register_capture"},{"independent_input_bits",true},{"target_ps",t}};
}
}
