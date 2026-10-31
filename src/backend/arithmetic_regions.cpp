#include "backend/arithmetic_regions.hpp"
#include "backend/beopt_dce.hpp"
#include <algorithm>
#include <map>
#include <queue>
#include <set>
#include <sstream>
#include <stdexcept>

namespace pred::beir::regions {
namespace {
[[noreturn]] void bad(const std::string& s) { throw std::runtime_error("arithmetic region: " + s); }
Operand ref(const Program& p, NodeId id) {
    Operand v; v.node=id; v.type=p.signal(id).type; return v;
}
void clearNodeLineage(Signal& s) {
    s.debug.derived_nodes.clear();
    if(s.driver) s.driver->debug.derived_nodes.clear();
}
void remapLineage(DebugInfo& d,const std::map<NodeId,NodeId>& map) {
    std::vector<NodeId> nodes;
    for(auto id:d.derived_nodes) {auto it=map.find(id);if(it!=map.end()) nodes.push_back(it->second);}
    d.derived_nodes=std::move(nodes);
}
void canonicalizeSymbolNames(Program& p) {
    auto fix=[&](Operand& v) {if(v.kind==OperandKind::Symbol) v.text=p.signal(v.node).name;};
    for(auto& s:p.signals) if(s.driver) for(auto& v:s.driver->operands) fix(v);
    for(auto& r:p.arithmetic_regions) for(auto& v:r.inputs) fix(v);
}
std::vector<int> owners(const Program& p) {
    std::vector<int> o(p.signals.size(),0);
    std::set<std::string> ids;
    for (std::size_t i=0;i<p.arithmetic_regions.size();++i) {
        const auto& r=p.arithmetic_regions[i];
        if(r.id.empty() || !ids.insert(r.id).second) bad("empty or duplicate stable identity");
        if(r.members.empty() || r.outputs.empty()) bad("empty members or exits: "+r.id);
        if(!r.internal_split_enabled && !r.legal_cut_groups.empty())
            bad("internal cuts are not enabled for this atomic region");
        if (r.internal_split_enabled && (r.operation != "mul_unsigned" || r.template_version != "mul-block-tree-v1" || r.split_version != "partial-products-reduction-levels-v1"))
            bad("unsupported internal split policy");
        if (r.internal_split_enabled && r.legal_cut_groups.empty())
            bad("split-enabled region has no legal groups");
        std::set<NodeId> grouped;
        for (const auto& group : r.legal_cut_groups) {
            if (group.empty()) bad("empty legal split group");
            for (auto id : group) {
                if (!std::count(r.members.begin(), r.members.end(), id))
                    bad("legal split group contains a non-member");
                if (!grouped.insert(id).second) bad("overlapping legal split groups");
            }
        }
        if (r.internal_split_enabled && grouped.size() != r.members.size())
            bad("legal split groups do not cover all members");
        for(auto id:r.members) {
            if(id>=o.size() || o[id]) bad("invalid, duplicate or overlapping member");
            o[id]=static_cast<int>(i)+1;
        }
        if(!r.internal_split_enabled && !r.internal_registers.empty()) bad("internal registers in atomic region");
        for(auto id:r.internal_registers) {
            if(id>=o.size() || o[id]) bad("invalid or overlapping internal register");
            const auto& s=p.signal(id);
            if(!s.driver || s.driver->kind!=OperationKind::Assign || s.driver->operands.size()!=1)
                bad("malformed internal register driver");
            o[id]=static_cast<int>(i)+1;
        }
    }
    return o;
}
void dag(const std::vector<std::set<NodeId>>& edges) {
    std::vector<unsigned> deg(edges.size());
    for(const auto& es:edges) for(auto v:es) ++deg.at(v);
    std::queue<NodeId> q;
    for(NodeId i=0;i<deg.size();++i) if(!deg[i]) q.push(i);
    std::size_t count=0;
    while(!q.empty()) { auto u=q.front();q.pop();++count;
        for(auto v:edges[u]) if(!--deg[v]) q.push(v);
    }
    if(count!=edges.size()) bad("cyclic graph or external path through contracted region");
}
}
std::vector<std::vector<NodeId>> schedulingGroups(const Program& p) {
    std::vector<std::vector<NodeId>> groups;
    for(const auto& r:p.arithmetic_regions) {
        if(r.internal_split_enabled) groups.insert(groups.end(),r.legal_cut_groups.begin(),r.legal_cut_groups.end());
        else groups.push_back(r.members);
    }
    return groups;
}
void validate(const Program& p) {
    auto o=owners(p);
    std::vector<std::set<NodeId>> edges(p.signals.size());
    std::vector<NodeId> unit(o.size()); NodeId n=p.arithmetic_regions.size();
    for(NodeId i=0;i<o.size();++i) unit[i]=o[i]?o[i]-1:n++;
    std::vector<std::set<NodeId>> contracted(n);
    std::vector<std::set<NodeId>> ins(p.arithmetic_regions.size()), outs(ins.size());
    for(std::size_t i=0;i<ins.size();++i) {
        const auto& r=p.arithmetic_regions[i];
        for(const auto& v:r.inputs) if(v.kind==OperandKind::Symbol) {
            if(v.node>=o.size() || o[v.node]==static_cast<int>(i)+1) bad("invalid input boundary "+r.id+" #"+std::to_string(v.node));
            if(v.type.width!=p.signal(v.node).type.width || v.type.array_dims!=p.signal(v.node).type.array_dims) bad("mistyped input boundary");
            ins[i].insert(v.node);
        } else if(v.kind!=OperandKind::Literal) bad("raw port region input");
        for(auto id:r.outputs) {
            if(id>=o.size() || o[id]!=static_cast<int>(i)+1 || !outs[i].insert(id).second) bad("invalid exit boundary");
        }
    }
    for(NodeId i=0;i<p.signals.size();++i) {
        const auto& s=p.signals[i];
        if(s.id!=i || s.type.width<=0) bad("non-dense ID or invalid type");
        if(o[i] && !s.port_name.empty()) bad("public port inside region");
        if(!s.driver) { if(o[i]) bad("undriven region member"); continue; }
        if(s.driver->kind==OperationKind::F32MulRNE) bad("unexpanded semantic marker");
        if(o[i] && s.driver->kind==OperationKind::Call) bad("opaque call inside region");
        for(const auto& v:s.driver->operands) if(v.kind==OperandKind::Symbol) {
            if(v.node>=o.size() || v.type.width!=p.signal(v.node).type.width || v.type.array_dims!=p.signal(v.node).type.array_dims) bad("dangling or mistyped operand");
            edges[v.node].insert(i);
            if(unit[v.node]!=unit[i]) contracted[unit[v.node]].insert(unit[i]);
            if(o[i]!=o[v.node]) {
                if(o[i] && !ins[o[i]-1].count(v.node)) bad("undeclared entry");
                if(o[v.node] && !outs[o[v.node]-1].count(v.node)) bad("internal node escaped region");
            }
        } else if(o[i] && v.kind==OperandKind::Port) bad("port read inside region");
    }
    for(const auto& port:p.ports) for(auto id:port.element_nodes) {
        if(id>=o.size() || o[id]) bad("invalid public port boundary");
    }
    dag(edges); dag(contracted);
    // A partition may create a cycle even when the original node DAG is legal.
    std::vector<NodeId> group_of(p.signals.size(), kInvalidNodeId);
    NodeId group_count=0;
    for(const auto& group:schedulingGroups(p)) {
        for(auto id:group) group_of[id]=group_count;
        ++group_count;
    }
    for(auto& id:group_of) if(id==kInvalidNodeId) id=group_count++;
    std::vector<std::set<NodeId>> group_edges(group_count);
    for(NodeId source=0;source<edges.size();++source) for(auto user:edges[source])
        if(group_of[source]!=group_of[user]) group_edges[group_of[source]].insert(group_of[user]);
    dag(group_edges);
}

Program expandF32(Program p, const Program& implementation) {
    std::vector<NodeId> markers;
    for(const auto& s:p.signals) if(s.driver && s.driver->kind==OperationKind::F32MulRNE) markers.push_back(s.id);
    NodeId a=kInvalidNodeId,b=a,y=a;
    for(const auto& port:implementation.ports) {
        if(port.element_nodes.size()!=1) bad("template requires scalar ports");
        if(port.name=="a") a=port.element_nodes[0];
        if(port.name=="b") b=port.element_nodes[0];
        if(port.name=="y") y=port.element_nodes[0];
    }
    if(a==kInvalidNodeId || b==kInvalidNodeId || b==a || y==kInvalidNodeId) bad("invalid FP implementation interface");
    for(auto id:markers) {
        const auto marker=*p.signal(id).driver;
        if(marker.operands.size()!=2 || p.signal(id).type.width!=37 || marker.arithmetic_origin.empty()) bad("malformed FP marker");
        ArithmeticRegion r; r.id=marker.arithmetic_origin; r.inputs=marker.operands;
        // Preserve the original marker as an outer alias; the semantic exit is
        // a separate owned anchor, so public ports never become region members.
        std::vector<Operand> map(implementation.signals.size());
        map[a]=marker.operands[0];map[b]=marker.operands[1];
        for(const auto& s:implementation.signals) if(s.id!=a && s.id!=b) {
            Signal copy=s; copy.id=p.signals.size();copy.name="arith_"+std::to_string(id)+"_"+std::to_string(s.id);
            copy.port_name.clear();copy.port_element_index=-1;copy.value={};clearNodeLineage(copy);
            map[s.id]=ref(implementation,s.id);map[s.id].node=copy.id;
            r.members.push_back(copy.id);p.signals.push_back(std::move(copy));
        }
        for(auto member:r.members) {
            auto& s=p.signal(member);
            if(!s.driver) bad("undriven implementation node");
            for(auto& v:s.driver->operands) if(v.kind==OperandKind::Symbol) {
                bool sv=v.signed_view;v=map.at(v.node);v.signed_view=sv;
            } else if(v.kind==OperandKind::Port) bad("unexpected implementation port");
        }
        r.outputs={map[y].node};
        Operation alias;alias.type=p.signal(id).type;alias.operands={map[y]};
        p.signal(id).driver=alias;p.signal(id).value={};
        p.arithmetic_regions.push_back(std::move(r));
    }
    p.temp_names_initialized=false;
    canonicalizeSymbolNames(p);validate(p);return p;
}

Program optimizeAtomic(Program p,const opt::Options& options,const opt::IterationCallback& callback) {
    validate(p);
    for(const auto& r:p.arithmetic_regions) if(r.internal_split_enabled)
        bad("split templates require boundary-preserving DCE/facts cleanup; generic BEOPT rejected");
    auto owner=owners(p);
    // Optimize disjoint components with explicit import/export ports. Synthetic
    // aliases keep semantic exits present even when their values become literals.
    std::vector<std::set<NodeId>> exports(p.arithmetic_regions.size()+1);
    std::set<NodeId> public_inputs;
    for(const auto& port:p.ports) for(auto id:port.element_nodes) {
        if(port.direction==PortDirection::Input) public_inputs.insert(id);
        else exports[0].insert(id);
    }
    for(const auto& r:p.arithmetic_regions) for(auto id:r.outputs) exports[owner[id]].insert(id);
    for(const auto& s:p.signals) if(s.driver) for(const auto& v:s.driver->operands)
        if(v.kind==OperandKind::Symbol && owner[v.node]!=owner[s.id]) exports[owner[v.node]].insert(v.node);
    for(const auto& r:p.arithmetic_regions) for(const auto& v:r.inputs)
        if(v.kind==OperandKind::Symbol) exports[owner[v.node]].insert(v.node);
    Program assembled=p;
    for(auto& s:assembled.signals) s.driver.reset();
    for(auto& r:assembled.arithmetic_regions) r.members.clear();
    for(std::size_t component=0;component<exports.size();++component) {
        Program local;local.function_name=p.function_name;
        std::map<NodeId,NodeId> copied, imports;
        std::map<std::string,NodeId> input_globals, output_globals;
        auto portNode=[&](NodeId global,bool real) {
            Signal s=p.signal(global);s.id=local.signals.size();s.value={};clearNodeLineage(s);
            s.name=real?s.name:"__arith_import_"+std::to_string(global);
            s.port_name=s.name;
            Operation op;op.kind=OperationKind::PortRead;op.type=s.type;
            Operand v;v.kind=OperandKind::Port;v.text=s.name;v.type=s.type;op.operands={v};s.driver=op;
            Port port;port.name=s.name;port.direction=PortDirection::Input;port.type=s.type;port.element_nodes={s.id};
            local.ports.push_back(port);local.inputs.push_back(s.name);input_globals[s.name]=global;
            local.signals.push_back(s);return s.id;
        };
        for(const auto& s:p.signals) if(owner[s.id]==static_cast<int>(component)) {
            if(public_inputs.count(s.id)) copied[s.id]=portNode(s.id,true);
            else { Signal c=s;c.id=local.signals.size();c.port_name.clear();c.value={};clearNodeLineage(c);
                copied[s.id]=c.id;local.signals.push_back(std::move(c)); }
        }
        // Collect first: appending imports must not invalidate signal references.
        for(const auto& s:p.signals) if(owner[s.id]==static_cast<int>(component) && s.driver)
            for(const auto& v:s.driver->operands) if(v.kind==OperandKind::Symbol && owner[v.node]!=static_cast<int>(component) && !imports.count(v.node))
                imports[v.node]=portNode(v.node,false);
        for(const auto& [global,id]:copied) {
            if(public_inputs.count(global)) continue;
            auto& s=local.signal(id);
            if(s.driver) for(auto& v:s.driver->operands) if(v.kind==OperandKind::Symbol)
                v.node=owner[v.node]==static_cast<int>(component)?copied.at(v.node):imports.at(v.node);
        }
        for(auto global:exports[component]) {
            if(public_inputs.count(global)) continue;
            Signal s;s.id=local.signals.size();s.name="__arith_export_"+std::to_string(global);s.port_name=s.name;s.type=p.signal(global).type;
            Operation op;op.type=s.type;op.operands={ref(local,copied.at(global))};s.driver=op;
            Port port;port.name=s.name;port.direction=PortDirection::Output;port.type=s.type;port.element_nodes={s.id};
            output_globals[s.name]=global;local.outputs.push_back(s.name);local.ports.push_back(port);local.signals.push_back(s);
        }
        canonicalizeSymbolNames(local);
        local=opt::optimizeProgram(std::move(local),options,callback);
        std::map<NodeId,NodeId> remap;
        std::set<NodeId> imported;
        for(const auto& port:local.ports) {
            auto id=port.element_nodes.at(0);
            if(port.direction==PortDirection::Input) {
                remap[id]=input_globals.at(port.name);
                if(component || !public_inputs.count(remap[id])) imported.insert(id);
            } else remap[id]=output_globals.at(port.name);
        }
        for(const auto& s:local.signals) if(!remap.count(s.id)) {
            remap[s.id]=assembled.signals.size();
            Signal blank;blank.id=assembled.signals.size();assembled.signals.push_back(blank);
        }
        for(const auto& s:local.signals) {
            if(imported.count(s.id)) continue;
            auto id=remap.at(s.id);Signal c=s;c.id=id;c.value={};c.port_name=p.signals.size()>id?p.signal(id).port_name:"";
            c.name=p.signals.size()>id?p.signal(id).name:"arith_opt_"+std::to_string(id);
            if(c.driver) for(auto& v:c.driver->operands) if(v.kind==OperandKind::Symbol) v.node=remap.at(v.node);
            remapLineage(c.debug,remap);if(c.driver) remapLineage(c.driver->debug,remap);
            assembled.signal(id)=std::move(c);
            if(component) assembled.arithmetic_regions[component-1].members.push_back(id);
        }
    }
    assembled.temp_names_initialized=false;
    canonicalizeSymbolNames(assembled);
    MutableProgram graph(std::move(assembled));
    opt::eliminateDeadNodes(graph);
    assembled=graph.finish();validate(assembled);return assembled;
}

std::string report(const Program& p) {
    std::ostringstream os;os<<"arithmetic-regions atomic-v1 count="<<p.arithmetic_regions.size()<<"\n";
    for(const auto& r:p.arithmetic_regions) {
        os<<"region "<<r.id<<" operation="<<r.operation<<" semantics="<<r.semantics<<" template="<<r.template_version
          <<" sharing="<<r.sharing_policy<<" split="<<(r.internal_split_enabled?"enabled":"disabled")
          <<" split_version="<<r.split_version<<" restoration_clones=0 cuts="<<r.legal_cut_groups.size()<<" members="<<r.members.size()<<" inputs=";
        for(const auto& v:r.inputs) os<<(v.kind==OperandKind::Symbol?"#"+std::to_string(v.node):"literal")<<",";
        os<<" outputs=";for(auto id:r.outputs) os<<"#"<<id<<",";os<<"\n";
        os<<"  member_ids=";for(auto id:r.members) os<<"#"<<id<<",";os<<"\n";
        os<<"  internal_register_ids=";for(auto id:r.internal_registers) os<<"#"<<id<<",";os<<"\n";
    }return os.str();
}
}
