#include "backend/arithmetic_operator_query.hpp"
#include "backend/pipeline.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static Operand ref(const Program& p,NodeId id){Operand a;a.kind=OperandKind::Symbol;a.node=id;a.type=p.signal(id).type;return a;}
static NodeId add(Program& p,int width,OperationKind kind,std::vector<Operand> operands={},OpCode op=OpCode::None){Signal s;s.id=p.signals.size();s.name="n"+std::to_string(s.id);s.type.width=width;Operation d;d.kind=kind;d.type=s.type;d.op=op;d.operands=std::move(operands);s.driver=d;p.signals.push_back(s);return s.id;}
static NodeId input(Program& p,int width){Operand a;a.kind=OperandKind::Port;a.type.width=width;a.text="in"+std::to_string(p.signals.size());auto at=add(p,width,OperationKind::PortRead,{a});Port port;port.name=a.text;port.type.width=width;port.direction=PortDirection::Input;port.element_nodes={at};p.ports.push_back(port);p.inputs.push_back(port.name);return at;}
static void output(Program& p,NodeId value){auto at=add(p,p.signal(value).type.width,OperationKind::Assign,{ref(p,value)});Port port;port.name="out"+std::to_string(at);port.type=p.signal(at).type;port.direction=PortDirection::Output;port.element_nodes={at};p.ports.push_back(port);p.outputs.push_back(port.name);}
static Program graph(int lhs,int rhs,int result,bool carry,bool zero_pad,int dead=0){Program p;p.function_name="unseen_parameter_graph";if(dead)input(p,1);auto a=input(p,zero_pad?lhs-3:lhs),b=input(p,rhs);if(zero_pad){Operand z;z.kind=OperandKind::Literal;z.type.width=3;z.constant.width=3;z.constant.limbs={0};a=add(p,lhs,OperationKind::Concat,{ref(p,a),z});}auto id=carry?add(p,result,OperationKind::AddCarry,{ref(p,a),ref(p,b),ref(p,input(p,1))}):add(p,result,OperationKind::Binary,{ref(p,a),ref(p,b)},zero_pad?OpCode::Add:OpCode::Mul);output(p,id);return p;}
int main(int argc,char** argv){try{
    check(argc==2,"new output required");std::filesystem::path out=argv[1];check(!std::filesystem::exists(out),"preserve prior tests");std::filesystem::create_directories(out);A rows;int invariance=0;
    struct Shape{int lhs,rhs,result;bool carry,padding;};
    for(auto shape:{Shape{12,20,32,false,false},Shape{13,17,11,false,false},Shape{17,29,30,false,true},Shape{63,65,66,true,false}}){
        auto p=graph(shape.lhs,shape.rhs,shape.result,shape.carry,shape.padding);std::vector<int> stages(p.signals.size(),0);auto queries=operatorQueries(p,stages,0,1373,"TEST_ONLY-profile","TEST_ONLY-boundary");check(queries.size()==1,"one real operator expected");const auto& q=queries.front();
        check(q.features.at("lhs_width")==shape.lhs&&q.features.at("rhs_width")==shape.rhs&&q.features.at("result_width")==shape.result,"unseen operand/result extraction");
        if(shape.padding)check(q.features.at("lhs_zero_lsb_run")==3,"known zero position lost");
        if(shape.carry)check(q.features.at("carry_dynamic")==1,"dynamic carry lost");
        auto changed=graph(shape.lhs,shape.rhs,shape.result,shape.carry,shape.padding,1);changed.function_name="unrelated_application";for(auto& s:changed.signals)s.name="renamed_"+s.name;
        auto other=operatorQueries(changed,std::vector<int>(changed.signals.size(),0),0,1373,"TEST_ONLY-profile","TEST_ONLY-boundary");check(other.size()==1&&other[0].features==q.features,"case/NodeId leaked into numerical features");++invariance;
        auto parsed=llvm::json::parse(operatorQueryJson(q));check(bool(parsed),"query serialization");rows.push_back(std::move(*parsed));
    }
    // Two direct outputs of one value must be charged as banks, not as two
    // producer instances. Query captures expose this before any mapped count.
    auto shared=graph(12,20,32,false,false);auto original=shared.ports.back().element_nodes.front();output(shared,original);
    auto qs=operatorQueries(shared,std::vector<int>(shared.signals.size(),0),0,1500,"TEST_ONLY-profile","TEST_ONLY-boundary");check(qs.size()==1&&qs[0].features.at("capture_banks")==2,"shared producer duplicate-bank projection");
    rows.push_back(std::move(*llvm::json::parse(operatorQueryJson(qs[0]))));
    bool refused=false;auto bad=graph(12,20,32,false,false);for(auto& s:bad.signals)if(s.driver&&s.driver->kind==OperationKind::Binary)s.driver->operands[0].signed_view=true;
    try{operatorQueries(bad,std::vector<int>(bad.signals.size(),0),0,1500,"TEST_ONLY-profile","TEST_ONLY-boundary");}catch(const rtlzz::ArithmeticError&){refused=true;}check(refused,"signed data accepted");
    std::ofstream(out/"proof.json")<<llvm::formatv("{0:2}",V(O{{"status","passed"},{"queries",std::move(rows)},{"independent_parameter_contexts",4},{"NodeId_and_name_invariance",invariance},{"shared_producer_capture_checked",true},{"signed_operand_rejected",true},{"physical_qualification",false}})).str()<<'\n';
    std::cout<<"OPERATOR_QUERY_CONTRACT_PASS shapes=4 rename_and_renumber="<<invariance<<" shared_capture=1 signed_rejection=1\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
