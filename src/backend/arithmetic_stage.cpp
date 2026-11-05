#include "backend/arithmetic_core.hpp"
#include <algorithm>
#include <map>
#include <set>
#include <sstream>

namespace pred::beir::arithmetic {
StageCostQuery stageCostQuery(const Program& p,const std::vector<int>& placement,int index,int last,
                             const std::vector<NodeId>& controls) {
    auto fail=[](const std::string& text)->void {
        throw rtlzz::ArithmeticError({"cost","cost_uncovered",text,{},{}});
    };
    if(placement.size()!=p.signals.size()||index<0||index>last)fail("invalid actual stage placement");
    StageCostQuery q;q.index=index;q.last=last;q.placement=placement;q.controls=controls;
    std::set<NodeId> ignored(controls.begin(),controls.end()),inside;
    for(auto id:ignored)if(id>=p.signals.size())fail("invalid stage control node");
    if(!controls.empty()) {
        if(controls.size()!=2||ignored.size()!=2)fail("valid controls require distinct input/output nodes");
        const auto& input=p.signal(controls[0]);const auto& output=p.signal(controls[1]);
        if(input.type.width!=1||output.type.width!=1||input.type.isArray()||output.type.isArray()||
           !input.driver||input.driver->kind!=OperationKind::PortRead||!output.driver)fail("invalid scalar valid protocol cut");
        bool input_port=false,output_port=false;
        for(const auto& port:p.ports)for(auto id:port.element_nodes) {
            input_port|=id==input.id&&port.direction==PortDirection::Input;
            output_port|=id==output.id&&port.direction==PortDirection::Output;
        }
        if(!input_port||!output_port)fail("valid controls must bind actual ports");
        auto id=output.id;std::set<NodeId> visited;
        while(id!=input.id&&!(p.signal(id).value.valid&&p.signal(id).value.constant)) {
            if(!visited.insert(id).second)fail("cyclic valid protocol alias");
            const auto& d=*p.signal(id).driver;
            if((d.kind!=OperationKind::Assign&&d.kind!=OperationKind::Cast)||d.operands.size()!=1)fail("arithmetic cannot be excluded as valid control");
            if(d.operands[0].kind==OperandKind::Literal)break;
            if(d.operands[0].kind!=OperandKind::Symbol||d.operands[0].type.width!=1)fail("invalid valid protocol alias");
            id=d.operands[0].node;
        }
    }
    std::set<NodeId> live;
    std::vector<NodeId> pending;
    for(const auto& port:p.ports)pending.insert(pending.end(),port.element_nodes.begin(),port.element_nodes.end());
    for(const auto& region:p.arithmetic_regions){pending.insert(pending.end(),region.outputs.begin(),region.outputs.end());for(const auto& a:region.inputs)if(a.kind==OperandKind::Symbol)pending.push_back(a.node);}
    while(!pending.empty()) {auto id=pending.back();pending.pop_back();if(!live.insert(id).second)continue;
        const auto& s=p.signal(id);if(!s.driver)fail("undriven stage boundary");
        for(const auto& a:s.driver->operands)if(a.kind==OperandKind::Symbol)pending.push_back(a.node);
    }
    for(const auto& s:p.signals) {
        if(s.id>=placement.size()||placement[s.id]<0||placement[s.id]>last||!s.driver)fail("invalid stage graph/member");
        if(!live.count(s.id))continue;
        for(const auto& a:s.driver->operands)if(a.kind==OperandKind::Symbol) {
            if(a.node>=placement.size()||placement[a.node]>placement[s.id])fail("stage placement violates a dependency");
        }
        if(placement[s.id]==index&&!ignored.count(s.id)&&s.driver->kind!=OperationKind::PortRead&&
           !(s.value.valid&&s.value.constant))inside.insert(s.id);
    }
    q.members.assign(inside.begin(),inside.end());
    std::set<NodeId> atomic_outputs;
    for(const auto& region:p.arithmetic_regions)if(!region.internal_split_enabled)
        atomic_outputs.insert(region.outputs.begin(),region.outputs.end());
    auto canonical=[&](NodeId id) {
        std::set<NodeId> seen;
        while(inside.count(id)) {
            if(!seen.insert(id).second)fail("cyclic capture alias");
            if(atomic_outputs.count(id))break;
            const auto& s=p.signal(id);const auto& d=*s.driver;
            if((d.kind!=OperationKind::Assign&&d.kind!=OperationKind::Cast)||d.operands.size()!=1||
               d.operands[0].kind!=OperandKind::Symbol||d.operands[0].type.width!=s.type.width)break;
            id=d.operands[0].node;
        }
        return id;
    };
    // Crossing consumers share the first physical register in a shared chain.
    // Distinct external capture ports are separate logical banks.
    std::set<NodeId> crossing;
    std::map<NodeId,unsigned> external;
    for(const auto& s:p.signals)if(live.count(s.id)&&placement[s.id]>index&&!ignored.count(s.id))
        for(const auto& a:s.driver->operands)if(a.kind==OperandKind::Symbol&&inside.count(a.node))
            crossing.insert(canonical(a.node));
    if(index==last)for(const auto& port:p.ports)if(port.direction==PortDirection::Output)
        for(auto id:port.element_nodes)if(!ignored.count(id)&&inside.count(id))++external[canonical(id)];
    std::map<NodeId,unsigned> banks=external;
    for(auto id:crossing)banks[id]=std::max(1u,banks[id]);
    for(auto [id,count]:banks)q.captures.push_back({id,count});
    q.graph_sha256=rtlzz::arithmeticHash(emitText(p));
    std::ostringstream identity;identity<<q.graph_sha256<<'/'<<index<<'/'<<last;
    for(int at:placement)identity<<'/'<<at;
    for(auto id:q.controls)identity<<"/control:"<<id;
    for(auto capture:q.captures)identity<<"/capture:"<<capture.value<<':'<<capture.banks;
    q.cut_sha256=rtlzz::arithmeticHash(identity.str());
    return q;
}
}
