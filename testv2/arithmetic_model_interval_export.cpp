#include "backend/arithmetic_core.hpp"
#include "backend/mul_split.hpp"
#include "backend/beir.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace pred::beir;
static Operand ref(const Program& p,NodeId id){Operand o;o.node=id;o.type=p.signal(id).type;return o;}
int main(int argc,char**argv){
 if(argc<4){std::cerr<<"root out width...\n";return 1;}std::filesystem::path root=argv[1],out=argv[2];std::filesystem::create_directories(out);
 rtlzz::ArithmeticRequest request;request.target_platform="asap7";request.cost_mode="fixed";request.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();request.candidate_policy="core-original-v1";
 unsigned count=0;
 for(int i=3;i<argc;++i){int w=std::stoi(argv[i]);for(bool low:{false,true})for(int divisor:{2,4}){
  int block=std::max(2,w/divisor);if(divisor==4&&block==std::max(2,w/2))continue;
  Program p;p.function_name="MulInterval";
  for(const char* name:{"a","b"}){Signal s;s.id=p.signals.size();s.name=name;s.port_name=name;s.port_element_index=0;s.type.width=w;Operation op;op.type=s.type;op.kind=OperationKind::PortRead;Operand o;o.kind=OperandKind::Port;o.text=name;o.type=s.type;op.operands={o};s.driver=op;p.signals.push_back(s);Port port;port.name=name;port.type=s.type;port.direction=PortDirection::Input;port.element_nodes={s.id};p.ports.push_back(port);p.inputs.push_back(name);}
  Signal product;product.id=p.signals.size();product.name="y";product.port_name="y";product.port_element_index=0;product.type.width=low?w:2*w;Operation op;op.type=product.type;op.kind=OperationKind::Binary;op.op=OpCode::Mul;op.arithmetic_origin="model-mul#0";op.operands={ref(p,0),ref(p,1)};product.driver=op;p.signals.push_back(product);Port port;port.name="y";port.type=product.type;port.direction=PortDirection::Output;port.element_nodes={product.id};p.ports.push_back(port);p.outputs.push_back("y");
  p=arithmetic::registerOriginalOperators(std::move(p));auto id=p.arithmetic_regions.at(0).id;p=mul_split::expand(std::move(p),{{id,block}});arithmetic::validate(p,"model_interval_export");
  auto file=out/("w"+std::to_string(w)+(low?"_low_":"_full_")+(divisor==2?"half":"quarter")+".json");std::ofstream(file)<<arithmetic::report(p,request,nullptr,{},"")<<"\n";++count;
 }}std::cout<<"Native template interval source graphs exported: "<<count<<"\n";
}
