#include "backend/arithmetic_reduction.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static void check(bool b,const char* why){if(!b)throw std::runtime_error(why);}
static Operand ref(const Program& p,NodeId n){Operand a;a.kind=OperandKind::Symbol;a.node=n;a.type=p.signal(n).type;return a;}
static NodeId node(Program& p,int w,OperationKind k,std::vector<Operand> a={},OpCode op=OpCode::None){Signal s;s.id=p.signals.size();s.name="n"+std::to_string(s.id);s.type.width=w;Operation d;d.kind=k;d.type=s.type;d.operands=std::move(a);d.op=op;s.driver=std::move(d);p.signals.push_back(s);return s.id;}
static NodeId input(Program& p,int w){Operand a;a.kind=OperandKind::Port;a.type.width=w;a.text="in"+std::to_string(p.signals.size());auto n=node(p,w,OperationKind::PortRead,{a});Port port;port.name=a.text;port.type.width=w;port.direction=PortDirection::Input;port.element_nodes={n};p.ports.push_back(port);p.inputs.push_back(port.name);return n;}
static void output(Program& p,NodeId n){auto x=node(p,p.signal(n).type.width,OperationKind::Assign,{ref(p,n)});Port port;port.name="out"+std::to_string(x);port.type=p.signal(x).type;port.direction=PortDirection::Output;port.element_nodes={x};p.ports.push_back(port);p.outputs.push_back(port.name);}
static Program graph(int inner,int outer,bool observed=false,bool repeated=false,bool mul=false,bool shift=false,int dead=0){
    Program p;p.function_name="independent_reduce";if(dead)input(p,1);auto a=input(p,8),b=input(p,8),c=input(p,outer);
    auto sum=node(p,inner,OperationKind::Binary,{ref(p,a),ref(p,b)},mul?OpCode::Mul:OpCode::Add);
    if(observed)output(p,sum);auto wire=sum;
    if(shift){Operand z;z.kind=OperandKind::Literal;z.type.width=3;z.constant.width=3;z.constant.limbs={0};wire=node(p,inner+3,OperationKind::Concat,{ref(p,sum),z});}
    auto root=node(p,outer,OperationKind::Binary,{ref(p,wire),ref(p,repeated?a:c)},OpCode::Add);output(p,root);return p;
}
int main(int argc,char** argv){try{check(argc==2,"proof output path required");A cases;int checks=0;
    for(auto [inside,outside,shift]:{std::tuple<int,int,bool>{9,16,false},{8,8,false},{9,24,true},{12,20,false}}){auto p=graph(inside,outside,false,false,false,shift);auto r=reductionRegions(p,std::vector<int>(p.signals.size(),0),0);check(r.size()==1,"safe independent sum not discovered");auto q=graph(inside,outside,false,false,false,shift,1);q.function_name="different_application";auto renamed=reductionRegions(q,std::vector<int>(q.signals.size(),0),0);check(renamed.size()==1&&r[0].features==renamed[0].features,"identity leaked into G features");
        cases.push_back(reductionRegionJson(r[0],1373));++checks;}
    for(auto p:{graph(8,16),graph(9,16,true),graph(9,16,false,true),graph(16,20,false,false,true),graph(8,24,false,false,false,true)}){
        check(reductionRegions(p,std::vector<int>(p.signals.size(),0),0).empty(),"unsafe/shared/correlated region fused");++checks;}
    auto cut=graph(9,16);std::vector<int> placement(cut.signals.size(),0);placement[4]=placement[5]=1;
    check(reductionRegions(cut,placement,1).empty(),"register-cut Add tree fused");++checks;
    auto multi=graph(9,16);output(multi,4);check(reductionRegions(multi,std::vector<int>(multi.signals.size(),0),0).empty(),"multi-capture region fused");++checks;
    Program aliases;aliases.function_name="aliased_launch_words";auto a=input(aliases,8);auto x=node(aliases,8,OperationKind::Assign,{ref(aliases,a)}),y=node(aliases,8,OperationKind::Assign,{ref(aliases,a)});auto c=input(aliases,16);
    auto sum=node(aliases,9,OperationKind::Binary,{ref(aliases,x),ref(aliases,y)},OpCode::Add);auto end=node(aliases,16,OperationKind::Binary,{ref(aliases,sum),ref(aliases,c)},OpCode::Add);output(aliases,end);
    std::vector<int> stages(aliases.signals.size(),0);stages[sum]=stages[end]=stages.back()=1;
    check(reductionRegions(aliases,stages,1).empty(),"pure aliases mistaken for independent launch bits");++checks;
    auto sliced=graph(9,16);auto shifted=node(sliced,8,OperationKind::Slice,{ref(sliced,3)});sliced.signal(shifted).driver->lo=1;sliced.signal(shifted).driver->hi=8;
    // Appending may reallocate the signal vector; reacquire the root operation.
    sliced.signal(4).driver->operands[0]=ref(sliced,shifted);
    check(reductionRegions(sliced,std::vector<int>(sliced.signals.size(),0),0).empty(),"non-low intermediate slice fused");++checks;
    auto base=graph(9,16);auto rs=reductionRegions(base,std::vector<int>(base.signals.size(),0),0);auto bad=rs[0].terms;bad[1][0]=bad[0][0];bool rejected=false;try{reductionFeatures(16,bad,1500);}catch(const std::exception&){rejected=true;}check(rejected,"correlated bit features accepted");++checks;
    std::ofstream(argv[1])<<llvm::formatv("{0:2}",V(O{{"status","passed"},{"checks",checks},{"cases",std::move(cases)},{"price_qualification",false}})).str()<<'\n';std::cout<<"REDUCTION_CONTRACT_PASS checks="<<checks<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
