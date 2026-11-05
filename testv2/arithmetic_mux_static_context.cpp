#include "backend/arithmetic_mux_context.hpp"
#include <iostream>
#include <string>
using namespace pred::beir;

static NodeId append(Program& p,int width,OperationKind kind,std::vector<Operand> operands={},OpCode op=OpCode::None){
    Signal s;s.id=p.signals.size();s.name="arbitrary_"+std::to_string(s.id);s.type.width=width;Operation d;d.kind=kind;d.op=op;d.type=s.type;d.operands=std::move(operands);s.driver=d;p.signals.push_back(s);return s.id;
}
static Operand ref(const Program& p,NodeId id){Operand a;a.kind=OperandKind::Symbol;a.node=id;a.type=p.signals.at(id).type;return a;}
static NodeId input(Program& p,int width){Operand port;port.kind=OperandKind::Port;port.type.width=width;port.text="in_"+std::to_string(p.signals.size());
    auto id=append(p,width,OperationKind::PortRead,{port});p.signals[id].port_name=port.text;
    Port binding;binding.name=port.text;binding.direction=PortDirection::Input;binding.type.width=width;binding.element_nodes={id};p.ports.push_back(binding);p.inputs.push_back(port.text);return id;}
static Operand constant(int width,const std::string& kind){Operand a;a.kind=OperandKind::Literal;a.type.width=width;a.constant.width=width;
    a.constant.limbs.assign((width+63)/64,kind=="ones"?UINT64_MAX:kind=="one"?1:0);return a;}
int main(int argc,char** argv){
    if(argc<4)return 2;const int w=std::stoi(argv[1]),banks=std::stoi(argv[2]);const std::string context=argv[3],mutation=argc>4?argv[4]:"";
    if(w<1||w>64||banks<1||banks>3)return 2;
    std::vector<std::string> kinds;if(context=="mixed")kinds={"mux","mux_zero","mux_one","mux_ones","eq_zero","or"};
    else {int n=std::stoi(context);if(n<2||n>4)return 2;kinds.assign(n,"mux");}
    Program p;p.function_name="MuxStaticStageProjection";std::vector<NodeId> members;std::vector<arithmetic::MuxStaticCapture> captures;NodeId first_select=kInvalidNodeId;
    for(const auto& kind:kinds){NodeId a=input(p,w),b=kInvalidNodeId,c=kInvalidNodeId,id=kInvalidNodeId;
        if(kind=="mux"||kind=="or")b=input(p,w);if(kind.rfind("mux",0)==0)c=input(p,1);
        if(first_select==kInvalidNodeId&&c!=kInvalidNodeId)first_select=c;
        if(kind=="eq_zero")id=append(p,1,OperationKind::Unary,{ref(p,a)},OpCode::LogicNot);
        else if(kind=="or")id=append(p,w,OperationKind::Binary,{ref(p,a),ref(p,b)},OpCode::BitOr);
        else id=append(p,w,OperationKind::Ite,{ref(p,c),ref(p,a),kind=="mux"?ref(p,b):constant(w,kind.substr(4))});
        members.push_back(id);captures.push_back({id,unsigned(banks)});
        Port out;out.name="out_"+std::to_string(id);out.direction=PortDirection::Output;out.type=p.signals[id].type;out.element_nodes={id};
        p.signals[id].port_name=out.name;p.ports.push_back(out);p.outputs.push_back(out.name);
    }
    auto query=members[0];
    if(mutation=="shared")p.signals[members[1]].driver->operands[0]=ref(p,first_select);
    if(mutation=="connected")p.signals[members[1]].driver->operands[1]=ref(p,query);
    if(mutation=="missing")captures.pop_back();
    if(mutation=="unknown")p.signals[members.back()].driver->kind=OperationKind::Call;
    if(mutation=="rename")for(auto& s:p.signals)s.name="renamed_"+std::to_string(p.signals.size()-s.id);
    if(mutation=="reorder")std::reverse(members.begin(),members.end());
    try{auto f=arithmetic::muxStaticContext(p,members,captures,query);std::cout<<"{\"covered\":true,\"features\":{";
        bool first=true;for(const auto& [name,value]:f){if(!first)std::cout<<',';first=false;std::cout<<'"'<<name<<"\":"<<value;}std::cout<<"}}\n";
    }catch(const std::exception& e){std::cout<<"{\"covered\":false,\"reason\":\""<<e.what()<<"\"}\n";}
}
