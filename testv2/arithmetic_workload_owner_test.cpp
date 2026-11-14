#include "backend/arithmetic_workload_provider.hpp"
#include "backend/rtlgen.hpp"
#include "backend/arithmetic_wide_query.hpp"
#include <algorithm>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace pred;using namespace beir;using namespace beir::arithmetic;
namespace {
void check(bool b,const char* message){if(!b)throw std::runtime_error(message);}
Operand ref(const Program& p,NodeId n){Operand a;a.node=n;a.type=p.signal(n).type;return a;}
NodeId node(Program& p,int w,OperationKind k,std::vector<Operand> args,OpCode op=OpCode::None){Signal s;s.id=p.signals.size();s.name="n"+std::to_string(s.id);s.type.width=w;
    Operation d;d.kind=k;d.op=op;d.type=s.type;d.operands=std::move(args);s.driver=std::move(d);p.signals.push_back(s);return s.id;}
NodeId input(Program& p,const std::string& name,int w){Operand a;a.kind=OperandKind::Port;a.text=name;a.type.width=w;auto n=node(p,w,OperationKind::PortRead,{a});p.signal(n).port_name=name;p.signal(n).name=name;
    Port port;port.name=name;port.type.width=w;port.direction=PortDirection::Input;port.element_nodes={n};p.ports.push_back(port);p.inputs.push_back(name);return n;}
void output(Program& p,const std::string& name,NodeId value){auto n=node(p,p.signal(value).type.width,OperationKind::Assign,{ref(p,value)});p.signal(n).port_name=name;
    Port port;port.name=name;port.type=p.signal(n).type;port.direction=PortDirection::Output;port.element_nodes={n};p.ports.push_back(port);p.outputs.push_back(name);}
template<class F>void rejects(F f){bool rejected=false;try{f();}catch(const std::exception&){rejected=true;}check(rejected,"invalid/unqualified path accepted");}
Operand literal(int width,std::uint64_t value){Operand a;a.kind=OperandKind::Literal;a.type.width=width;a.constant.width=width;a.constant.limbs={value};return a;}
pipeline::Program materialize(Program raw){MutableProgram facts(std::move(raw));facts.ensureValueFacts();pipeline::Options options;
    options.input_valid_name="iv";options.output_valid_name="ov";options.reset_enabled=true;auto graph=facts.finish();
    for(const auto& port:graph.ports)if(port.direction==PortDirection::Input)options.input_reset_values[port.name]=std::vector<std::uint64_t>((port.type.width+63)/64,0);
    return pipeline::build(graph,options);}
}
int main(int argc,char** argv){try{
    Program raw;raw.function_name="owner_contract";auto a=input(raw,"a",512),b=input(raw,"b",512),iv=input(raw,"iv",1);
    auto c=input(raw,"c",512),d=input(raw,"d",512),e=input(raw,"e",512);
    auto sum=node(raw,513,OperationKind::Binary,{ref(raw,a),ref(raw,b)},OpCode::Add);
    auto left=node(raw,514,OperationKind::Binary,{ref(raw,sum),ref(raw,c)},OpCode::Add);
    auto right=node(raw,514,OperationKind::Binary,{ref(raw,d),ref(raw,e)},OpCode::Add);
    auto root=node(raw,515,OperationKind::Binary,{ref(raw,left),ref(raw,right)},OpCode::Add);output(raw,"y",root);output(raw,"z",sum);output(raw,"ov",iv);
    MutableProgram facts(std::move(raw));facts.ensureValueFacts();auto graph=facts.finish();
    pipeline::Options options;options.input_valid_name="iv";options.output_valid_name="ov";options.reset_enabled=true;
    for(const auto& port:graph.ports)if(port.direction==PortDirection::Input)options.input_reset_values[port.name]=std::vector<std::uint64_t>((port.type.width+63)/64,0);
    auto actual=pipeline::build(graph,options);auto plan=workloadPlan(actual,1373.,"software-profile");
    check(plan.reductions.size()==1&&plan.reductions.front().width==515,"wide G owner not installed");
    check(!plan.reduction_owners.count(sum),"shared Add interior absorbed");
    auto row=std::find_if(plan.operators.begin(),plan.operators.end(),[&](const auto& row){return row.node==sum;});
    check(row!=plan.operators.end(),"shared source missing from owners");
    check(!row->predecessor_query,"G consumer projected as the old internal Add");
    auto rtl=rtlgen::emitSystemVerilogWithWorkloadOwners(actual,plan);
    check(rtl.find("rtlzz_owner_"+std::to_string(sum)+"(")!=std::string::npos,"shared source not emitted");
    WorkloadCostProvider costs(actual,plan,llvm::json::Object{});
    check(!costs.node(actual.combinational,sum).covered,"missing model accepted");
    check(costs.node(actual.combinational,a).covered,"exact input wire rejected");
    auto changed=actual.combinational;changed.signal(a).name="changed";
    check(!costs.node(changed,a).covered,"stale graph accepted");
    rejects([&]{costs.componentReport(changed);});
    auto cut=stageCostQuery(actual.combinational,actual.stages,0,0);
    check(costs.nodeInStage(actual.combinational,a,cut).covered,"valid stage rejected");
    cut.last=1;check(!costs.nodeInStage(actual.combinational,a,cut).covered,"wrong last stage accepted");
    cut.last=0;cut.cut_sha256="forged";check(!costs.nodeInStage(actual.combinational,a,cut).covered,"forged stage accepted");
    cut=stageCostQuery(actual.combinational,actual.stages,0,0);cut.captures[0].banks+=1;
    check(!costs.nodeInStage(actual.combinational,a,cut).covered,"forged capture bank accepted");
    rejects([&]{costs.stateArea(1,1,1,1);});rejects([&]{costs.dataBudget(false);});
    rejects([&]{costs.stageDelay(1,1,1,{});});rejects([&]{costs.stageArea(1,1,1,{});});
    auto stale=plan;stale.reductions[0].terms[0][0].bit^=1;
    rejects([&]{rtlgen::emitSystemVerilogWithWorkloadOwners(actual,stale);});
    rejects([&]{WorkloadCostProvider bad(actual,stale,llvm::json::Object{});});
    llvm::json::Object wrong{{"version","workload-model-bindings-v2"},{"profile_sha256","another-profile"}};
    rejects([&]{WorkloadCostProvider bad(actual,plan,wrong);});
    auto mixed=row->query;auto* consumers=mixed.getObject("local_context")->getArray("consumers");
    auto* bindings=(*consumers)[0].getAsObject()->getArray("bindings");(*bindings)[0].getAsObject()->operator[]("role")="selector";
    rejects([&]{projectWorkloadQuery(mixed);});
    Program edge;edge.function_name="owner_edges";auto x=input(edge,"x",13),y=input(edge,"y",13),valid=input(edge,"iv",1);
    auto cmp=node(edge,1,OperationKind::Binary,{ref(edge,x),ref(edge,y)},OpCode::Lt);
    auto adc=node(edge,14,OperationKind::AddCarry,{ref(edge,x),ref(edge,y),ref(edge,cmp)});
    auto mux0=node(edge,13,OperationKind::Ite,{ref(edge,cmp),literal(13,0x1555),ref(edge,x)});
    auto mux1=node(edge,13,OperationKind::Ite,{ref(edge,cmp),ref(edge,y),literal(13,0xaaa)});
    output(edge,"s",adc);output(edge,"m0",mux0);output(edge,"m1",mux1);output(edge,"ov",valid);
    auto edge_actual=materialize(std::move(edge));auto edge_plan=workloadPlan(edge_actual,1373.,"software-profile");
    auto compare=std::find_if(edge_plan.operators.begin(),edge_plan.operators.end(),[&](const auto& row){return row.node==cmp;});
    check(compare!=edge_plan.operators.end(),"compare owner absent");auto f=workloadQueryFeatures(compare->query);
    check(f.at("selector_pin_uses")==2&&f.at("carry_pin_uses")==1&&f.at("consumer_count")==3,"multi-role consumers lost");
    check(!compare->predecessor_query,"selector/carry projected into v1");
    auto edge_rtl=rtlgen::emitSystemVerilogWithWorkloadOwners(edge_actual,edge_plan);
    if(argc==2)std::ofstream(argv[1])<<edge_rtl;
    // A narrow modulo result widened before the outer Add must remain a node.
    Program wrap;wrap.function_name="wrap";auto wa=input(wrap,"a",8),wb=input(wrap,"b",8),wc=input(wrap,"c",8),wv=input(wrap,"iv",1);
    auto narrow=node(wrap,8,OperationKind::Binary,{ref(wrap,wa),ref(wrap,wb)},OpCode::Add);
    auto outer=node(wrap,10,OperationKind::Binary,{ref(wrap,narrow),ref(wrap,wc)},OpCode::Add);output(wrap,"y",outer);output(wrap,"ov",wv);
    check(workloadPlan(materialize(std::move(wrap)),1373.,"software-profile").reductions.empty(),"modulo-then-widen fused unsafely");
    std::cout<<"WORKLOAD_OWNER_CONTRACT_PASS wide_G=515 shared_interior=retained stale_graph_stage_emitter_provider=rejected static_state_fallback=rejected L=1 multi_role_consumers=3 modulo_then_widen=retained\n";
    return 0;
}catch(const std::exception& error){std::cerr<<error.what()<<'\n';return 1;}}
