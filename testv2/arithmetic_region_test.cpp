#include "pipelinev2/PipelineV2.h"
#include "backend/arithmetic_regions.hpp"
#include "backend/beopt.hpp"
#include "backend/rtlgen.hpp"
#include <iostream>
#include <fstream>
#include <filesystem>
#include <cstdlib>
#include <set>
#include <algorithm>
using namespace pred;
using namespace beir;
#define CHECK(x) do { if(!(x)) { std::cerr<<__LINE__<<": "<<#x<<"\n"; std::exit(1); } } while(false)
static unsigned checks=0;
static Operand ref(const beir::Program& p,NodeId n){Operand v;v.node=n;v.type=p.signal(n).type;return v;}
static NodeId node(beir::Program& p,OperationKind k,std::vector<Operand> args={},OpCode code=OpCode::None) {
    Signal s;s.id=p.signals.size();s.name="n"+std::to_string(s.id);s.type.width=32;
    Operation d;d.kind=k;d.op=code;d.type=s.type;d.operands=std::move(args);s.driver=d;p.signals.push_back(s);return s.id;
}
static void port(beir::Program& p,NodeId n,const std::string& name,PortDirection d) {
    p.signal(n).port_name=name;p.signal(n).name=name;
    Port v;v.name=name;v.direction=d;v.type=p.signal(n).type;v.element_nodes={n};p.ports.push_back(v);
    (d==PortDirection::Input?p.inputs:p.outputs).push_back(name);
}
static beir::Program toy() {
    beir::Program p;p.function_name="toy";
    for(auto name:{"a","b"}) {Operand v;v.kind=OperandKind::Port;v.text=name;v.type.width=32;auto n=node(p,OperationKind::PortRead,{v});port(p,n,name,PortDirection::Input);}
    for(unsigned i=0;i<2;++i) {
        auto u=node(p,OperationKind::Binary,{ref(p,0),ref(p,1)},OpCode::Add);
        auto v=node(p,OperationKind::Binary,{ref(p,0),ref(p,1)},OpCode::Add);
        auto y=node(p,OperationKind::Binary,{ref(p,u),ref(p,v)},OpCode::Mul);
        ArithmeticRegion r;r.id="toy/"+std::to_string(i);r.inputs={ref(p,0),ref(p,1)};r.outputs={y};r.members={u,v,y};p.arithmetic_regions.push_back(r);
    }
    for(unsigned i=0;i<2;++i) {auto n=node(p,OperationKind::Assign,{ref(p,p.arithmetic_regions[i].outputs[0])});port(p,n,"y"+std::to_string(i),PortDirection::Output);}
    return p;
}
static unsigned countOp(const beir::Program& p,OpCode code) {
    return std::count_if(p.signals.begin(),p.signals.end(),[&](const auto& s){return s.driver && s.driver->op==code;});
}
static void rejects(beir::Program p) {
    bool rejected=false;try{regions::validate(p);}catch(const std::runtime_error&){rejected=true;}
    CHECK(rejected);++checks;

}
static void metadataTests(std::ostream& evidence) {
    auto p=toy();regions::validate(p);auto ordinary=p;ordinary.arithmetic_regions.clear();
    ordinary=opt::optimizeProgram(ordinary);auto isolated=opt::optimizeProgram(p);
    CHECK(countOp(ordinary,OpCode::Add)==1);
    CHECK(countOp(isolated,OpCode::Add)==2);CHECK(countOp(isolated,OpCode::Mul)==2);
    CHECK(isolated.arithmetic_regions[0].id=="toy/0" && isolated.arithmetic_regions[1].id=="toy/1");
    CHECK(isolated.arithmetic_regions[0].outputs[0]!=p.arithmetic_regions[0].outputs[0]);regions::validate(isolated);
    evidence<<"internal CSE: ordinary internal Add=1, isolated internal Add=2 (each region internally merges its duplicate Add)\n";
    ++checks;
    pipeline::Options atomic_options; atomic_options.register_stages=1;
    auto scheduled=pipeline::build(isolated,atomic_options);
    CHECK(scheduled.report.baseline_delay==4); // parallel Add nodes, then Mul; not sum(members).
    auto tampered=scheduled;
    auto n=isolated.arithmetic_regions[0].members.front();
    tampered.stages[n]=1-tampered.stages[n];
    bool rejected=false;try{pipeline::validateProgram(tampered);}catch(const std::runtime_error&){rejected=true;}
    CHECK(rejected);++checks;
    for (int stages : {0, 1}) {
        pipeline::Options options; options.register_stages=stages;
        auto materialized=pipeline::build(isolated, options);
        pipeline::validateProgram(materialized);
        materialized.rtl_program.arithmetic_regions.clear();
        bool missing_rejected=false;
        try { pipeline::validateProgram(materialized); }
        catch (const std::runtime_error&) { missing_rejected=true; }
        CHECK(missing_rejected);++checks;

        auto plain=isolated; plain.arithmetic_regions.clear();
        auto ordinary_pipeline=pipeline::build(plain, options);
        CHECK(ordinary_pipeline.combinational.arithmetic_regions.empty());
        CHECK(ordinary_pipeline.rtl_program.arithmetic_regions.empty());
        pipeline::validateProgram(ordinary_pipeline);++checks;
    }
    evidence<<"materialized region metadata: clearing all RTL regions rejected at stages=0/1; ordinary no-region pipelines accepted\n";
    // Explicit permutation: identities do not use NodeIds, names or locations.
    auto reversed=isolated;auto size=reversed.signals.size();
    auto remap=[&](NodeId n){return size-1-n;};
    std::reverse(reversed.signals.begin(),reversed.signals.end());
    for(auto& s:reversed.signals) { s.id=remap(s.id);s.debug={};
        if(s.driver) {s.driver->debug={};for(auto& v:s.driver->operands) if(v.kind==OperandKind::Symbol) v.node=remap(v.node);}
    }
    for(auto& port:reversed.ports) for(auto& n:port.element_nodes) n=remap(n);
    for(auto& r:reversed.arithmetic_regions) {
        for(auto& v:r.inputs) if(v.kind==OperandKind::Symbol) v.node=remap(v.node);
        for(auto& n:r.outputs) n=remap(n);
        for(auto& n:r.members) { n=remap(n);reversed.signal(n).name="renamed_"+std::to_string(n); }
    }
    regions::validate(reversed);reversed=opt::optimizeProgram(reversed);regions::validate(reversed);
    CHECK(reversed.arithmetic_regions[0].id=="toy/0");++checks;
    auto again=opt::optimizeProgram(isolated);regions::validate(again);
    CHECK(again.arithmetic_regions[0].id=="toy/0" && countOp(again,OpCode::Mul)==2);++checks;
    {
        auto aliased=toy();auto external=node(aliased,OperationKind::Assign,{ref(aliased,0)});
        for(auto& r:aliased.arithmetic_regions) r.inputs[0]=ref(aliased,external);
        for(auto& s:aliased.signals) if(s.id!=external && s.driver)
            for(auto& v:s.driver->operands) if(v.kind==OperandKind::Symbol && v.node==0) v.node=external;
        regions::validate(aliased);MutableProgram graph(aliased);
        CHECK(graph.replaceAliases({{external,ref(aliased,0)}}));
        auto resolved=graph.finish();regions::validate(resolved);
        for(const auto& r:resolved.arithmetic_regions) CHECK(r.inputs[0].node==0);++checks;
    }
    {
        MutableProgram graph(p);std::unordered_set<NodeId> live;
        for(const auto& s:p.signals) if(s.id!=4) live.insert(s.id);
        bool rejected=false;try{graph.compact(live);}catch(const std::runtime_error&){rejected=true;}
        CHECK(rejected);++checks;
    }
    auto bad=p;bad.arithmetic_regions[1].members.push_back(2);rejects(bad);
    bad=p;bad.arithmetic_regions[1].id="toy/0";rejects(bad);
    bad=p;bad.arithmetic_regions[0].inputs.clear();rejects(bad);
    bad=p;bad.signal(8).driver->operands={ref(bad,2)};rejects(bad); // escaped internal
    bad=p;bad.arithmetic_regions[0].legal_cut_groups={{2}};rejects(bad);
    bad=p;bad.signal(2).driver->operands={ref(bad,4)};rejects(bad); // full DAG cycle
    // Full DAG remains acyclic, but R0 -> external -> R0 is illegal.
    bad=p;auto outside=node(bad,OperationKind::Assign,{ref(bad,4)});
    bad.arithmetic_regions[0].outputs={2};bad.signal(outside).driver->operands={ref(bad,2)};
    bad.signal(4).driver->operands[0]=ref(bad,outside);bad.arithmetic_regions[0].inputs.push_back(ref(bad,outside));
    bad.signal(8).driver->operands={ref(bad,2)};rejects(bad);
    // Constant exit survives folding and dense-ID remapping; unused internals disappear.
    p=toy();for(auto& r:p.arithmetic_regions) {
        Operand v;v.kind=OperandKind::Literal;v.type.width=32;v.constant.width=32;v.constant.limbs={7};
        auto& d=*p.signal(r.outputs[0]).driver;d.kind=OperationKind::Assign;d.op=OpCode::None;d.operands={v};
    }
    auto folded=opt::optimizeProgram(p);regions::validate(folded);
    for(const auto& r:folded.arithmetic_regions) {
        CHECK(r.members.size()==1);CHECK(folded.signal(r.outputs[0]).driver->operands[0].kind==OperandKind::Literal);
    }++checks;
    evidence<<"metadata: overlap/identity/entry/exit/cut/full-cycle/contracted-cycle rejected; constant exits and repeated BEOPT preserved\n";
}
static void atomic(const pipeline::Program& p) {
    pipeline::validateProgram(p);regions::validate(p.rtl_program);
    for(const auto& r:p.combinational.arithmetic_regions) {
        std::set<NodeId> m(r.members.begin(),r.members.end());auto stage=p.stages.at(r.outputs[0]);
        for(auto n:r.members) CHECK(p.stages[n]==stage);
        for(const auto& reg:p.registers) if(m.count(reg.source))
            CHECK(std::find(r.outputs.begin(),r.outputs.end(),reg.source)!=r.outputs.end());
    }
}
int main(int argc,char** argv) {
    CHECK(argc==3);std::filesystem::create_directories(argv[2]);
    std::ofstream evidence(std::string(argv[2])+"/regions.txt");
    metadataTests(evidence);
    const std::vector<std::pair<std::string,std::string>> cases={
        {"single","y=vularith::F32MulRNE(a,b);z=y;"},
        {"serial","auto t=wrap(a,b);y=t;z=wrap(Int<32>(t),c);"},
        {"parallel","y=wrap(a,b);z=wrap(c,d);"},
        {"shared","y=wrap(a,b);z=wrap(a,c);"},
        {"identical","y=wrap(a,b);z=wrap(a,b);"},
        {"constant","y=wrap(Int<32>(0x3f800000),Int<32>(0x40000000));z=y;"}
    };
    for(const auto& [name,body]:cases) {
        pipelinev2::PipelineConfig c;c.source_name="fp_region_test.logic.cpp";
        c.source_text="#include \"arithmetic.hpp\"\nInt<32> a,b,c,d; Int<37> y,z; bool iv,ov;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port c\n#pragma input_port d\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port z\n#pragma output_port ov\ninline Int<37> wrap(Int<32> x,Int<32> w){return vularith::F32MulRNE(x,w);}\nvoid "+name+"(){"+body+"ov=false;}";
        c.top_function=name;c.clang_args={"-std=c++20",std::string("-I")+argv[1]};c.output_kind=pipelinev2::OutputKind::Rtl;
        auto r=pipelinev2::compile(c);
        if(!r.ok()) {std::cerr<<r.error<<"\n"<<r.error_debug_text<<"\n";return 1;}
        CHECK(r.beir_program);auto& p=*r.beir_program;regions::validate(p);
        CHECK(p.arithmetic_regions.size()==((name=="single"||name=="constant")?1:2));
        std::ofstream(std::string(argv[2])+"/"+name+".sv")<<r.output_text;
        evidence<<"case "<<name<<"\n"<<regions::report(p);++checks;
        if(name=="identical") {
            auto unprotected=p;unprotected.arithmetic_regions.clear();unprotected=opt::optimizeProgram(unprotected);
            CHECK(countOp(unprotected,OpCode::Add)<countOp(p,OpCode::Add));
            evidence<<"real identical FP internals: protected Add="<<countOp(p,OpCode::Add)<<" unprotected Add="<<countOp(unprotected,OpCode::Add)<<"\n";
            ++checks;
        }

        // Atomic v1 has no group-dependent cuts. All callers use this invariant;
        // group-specific candidate generation is deliberately left to step5.
        for(char group:std::string("ABCD")) {
            pipeline::Options o;o.register_stages=3;o.input_valid_name="iv";o.output_valid_name="ov";o.reset_enabled=true;
            o.input_reset_values={{"a",{0x7f800000}},{"b",{0}},{"c",{0x3f800000}},{"d",{0}},{"iv",{0}}};
            o.operation_delay_weights[rtlzz::PipelineOperation::Multiply]=group-'A'+1;
            auto piped=pipeline::build(p,o);atomic(piped);++checks;
            if(group=='B') {
                auto rtl=piped;rtl.rtl_program.function_name=name+"_pipe";
                std::ofstream(std::string(argv[2])+"/"+name+"_pipe.sv")<<rtlgen::emitSystemVerilog(rtl);
                evidence<<pipeline::emitReport(piped);
            }
        }
        // A second full region-aware optimization changes dense IDs without changing identities.
        auto again=opt::optimizeProgram(p);regions::validate(again);
        CHECK(again.arithmetic_regions.size()==p.arithmetic_regions.size());
        for(unsigned i=0;i<p.arithmetic_regions.size();++i) CHECK(again.arithmetic_regions[i].id==p.arithmetic_regions[i].id);++checks;
    }
    std::cout<<"PASS arithmetic regions checks="<<checks<<"\n";
}
