#pragma once
#include "backend/arithmetic_general_state.hpp"
#include "backend/arithmetic_selection.hpp"
#include "backend/rtlgen.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iomanip>
#include <limits>
#include <sstream>

using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static std::string read(const std::filesystem::path& p){std::ifstream f(p);if(!f)throw std::runtime_error("read "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
static void check(bool ok,const std::string& why){if(!ok)throw std::runtime_error(why);}
static void write(const std::filesystem::path& p,V value){std::ofstream(p)<<llvm::formatv("{0:2}",value).str()<<'\n';}
static V parsed(const std::string& s){auto v=llvm::json::parse(s);check(bool(v),"JSON parse");return std::move(*v);}
static NodeId add(Program& p,int width,OperationKind kind,std::vector<Operand> operands={},OpCode code=OpCode::None){
    Signal s;s.id=p.signals.size();s.name="s"+std::to_string(s.id);s.type.width=width;Operation d;d.kind=kind;d.type=s.type;d.op=code;d.operands=std::move(operands);s.driver=d;p.signals.push_back(s);return s.id;}
static Operand ref(const Program& p,NodeId id){Operand a;a.kind=OperandKind::Symbol;a.node=id;a.type=p.signal(id).type;return a;}
static Operand literal(int w,std::uint64_t value){Operand a;a.kind=OperandKind::Literal;a.type.width=w;a.constant.width=w;a.constant.limbs={value};return a;}
static NodeId input(Program& p,int width,const std::string& name){Operand a;a.kind=OperandKind::Port;a.type.width=width;a.text=name;auto id=add(p,width,OperationKind::PortRead,{a});
    Port port;port.name=name;port.type.width=width;port.element_nodes={id};port.direction=PortDirection::Input;p.ports.push_back(port);p.inputs.push_back(name);p.signal(id).name=name;p.signal(id).port_name=name;return id;}
static NodeId output(Program& p,NodeId id,const std::string& name){auto n=add(p,p.signal(id).type.width,OperationKind::Assign,{ref(p,id)});
    Port port;port.name=name;port.type=p.signal(id).type;port.element_nodes={n};port.direction=PortDirection::Output;p.ports.push_back(port);p.outputs.push_back(name);p.signal(n).name=name;p.signal(n).port_name=name;return n;}
static NodeId slice(Program& p,NodeId id,int lo,int width){auto n=add(p,width,OperationKind::Slice,{ref(p,id)});p.signal(n).driver->lo=lo;p.signal(n).driver->hi=lo+width-1;return n;}
struct Fixture {Program p;pipeline::Options options;ServiceStateProtocol service;std::vector<int> stages;std::vector<NodeId> data;};
static Fixture fixture(int depth,int mode,int form=0){
    Fixture f;f.p.function_name="GeneralStateFixture";f.options.register_stages=depth;f.options.reset_enabled=true;f.options.input_valid_name="iv";f.options.output_valid_name="ov";
    // Abstract fixture scheduling only: wiring has no delay. This makes the
    // actually materialized cut match the sampled post-adapter S bank; no
    // backend target, model parameter or production D scheduling is changed.
    f.options.operation_delay_weights={{rtlzz::PipelineOperation::Slice,0},{rtlzz::PipelineOperation::Assign,0},
        {rtlzz::PipelineOperation::Concat,0},{rtlzz::PipelineOperation::ZeroExtend,0}};
    auto a=input(f.p,64,"a"),b=input(f.p,66,"b"),iv=input(f.p,1,"iv");
    f.options.input_reset_values={{"a",{0x83d52c97e6a4135bULL}},{"b",{0x6ab109d72e856c43ULL,2}},{"iv",{0}}};
    for(int j=0;j<3;++j){int w=j==0?16:j==1?(form==3?37:36):64;auto left=slice(f.p,a,(j*7)%(65-w),w),right=slice(f.p,b,j==2?2:(j*11)%(67-w),w);
        auto pair=add(f.p,w,OperationKind::Binary,{ref(f.p,left),ref(f.p,right)},form==1?OpCode::Add:OpCode::BitXor);auto value=pair;
        if(j==1&&form!=3){auto lo=slice(f.p,pair,0,17),hi=slice(f.p,pair,17,19);value=add(f.p,37,OperationKind::Concat,{ref(f.p,hi),literal(1,form==2?1:0),ref(f.p,lo)});}
        if(j==2&&form!=3)value=add(f.p,66,OperationKind::ZExt,{ref(f.p,pair)});
        f.data.push_back(value);output(f.p,value,"y"+std::to_string(j));
    }
    output(f.p,iv,"ov");MutableProgram facts(f.p);facts.ensureValueFacts();f.p=facts.finish();
    f.stages.assign(f.p.signals.size(),0);for(const auto& port:f.p.ports)if(port.direction==PortDirection::Output)for(auto id:port.element_nodes)f.stages[id]=depth;
    f.service.mode=mode;f.service.valid_write_enable=mode==2?"":"we_iv";
    f.service.data_write_enables={{"a",mode==1?"we_iv":"we_a"},{"b",mode==1?"we_iv":"we_b"}};
    f.service.external_data_unconditional=f.service.external_valid_unconditional=true;return f;
}
static void wrapper(const pipeline::Program& actual,const Fixture& f,const std::filesystem::path& out,const std::string& name){
    auto core=rtlgen::emitSystemVerilog(actual);std::ofstream(out/(name+"-core.sv"))<<core;
    auto q=generalStateCostQuery(actual,1119,f.service);std::ofstream w(out/(name+"-wrapper.sv"));
    w<<"module Wrapper(input wire clk,rstn,iv,we_a,we_b,we_iv,input wire [63:0] a,input wire [65:0] b,output reg ov,output reg [15:0] y0,output reg [36:0] y1,output reg [63:0] y2);\n";
    w<<"reg [63:0] launch_a;reg [65:0] launch_b;reg launch_valid;wire v;wire [15:0] t0;wire [36:0] t1;wire [63:0] t2;\n";
    auto reset_value=[](const std::vector<std::uint64_t>& x){std::ostringstream s;for(auto it=x.rbegin();it!=x.rend();++it)s<<std::hex<<std::setfill('0')<<std::setw(16)<<*it;auto value=s.str();auto first=value.find_first_not_of('0');return first==std::string::npos?std::string("0"):value.substr(first);};
    w<<"always @(posedge clk)begin if(!rstn)begin launch_a<=64'h"<<reset_value(f.options.input_reset_values.at("a"))<<";launch_b<=66'h"<<reset_value(f.options.input_reset_values.at("b"))<<";launch_valid<=0;end else begin\n";
    w<<"if("<<f.service.data_write_enables.at("a")<<")launch_a<=a;if("<<f.service.data_write_enables.at("b")<<")launch_b<=b;";
    if(f.service.mode!=2)w<<"if("<<f.service.valid_write_enable<<")";w<<"launch_valid<=iv;end end\n";
    w<<actual.combinational.function_name<<" core(.clk(clk),.rstn(rstn),.a(launch_a),.b(launch_b),.iv(launch_valid),.y0(t0),.y1(t1),.y2(t2),.ov(v));\n";
    w<<"always @(posedge clk)begin if(!rstn)begin ov<=0;";
    for(const auto& slot:q.slots)if(slot.owner=="external_state"&&!slot.state.valid)w<<slot.state.name<<"<="<<slot.state.declared_width<<"'h"<<reset_value(slot.state.reset_values)<<";";
    w<<"end else begin y0<=t0;y1<=t1;y2<=t2;ov<=v;end end\nendmodule\n";
}
