#include "rtlzz.hpp"
#include "pipelinev2/PipelineV2.h"
#include "backend/arithmetic_core.hpp"
#include "backend/arithmetic_regions.hpp"
#include "backend/pipeline.hpp"
#include <llvm/Support/JSON.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <algorithm>
#include <limits>
using namespace pred;
using namespace beir;
static unsigned checks=0;
static bool circt=false;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<__LINE__<<": "<<#x<<"\n";std::exit(1);}}while(false)
static std::string read(const std::string& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
static rtlzz::ArithmeticRequest request(const std::filesystem::path& root,int L=3){
    rtlzz::ArithmeticRequest r;r.target_platform="asap7";r.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();r.platform_profile_sha256=rtlzz::arithmeticHash(read(r.platform_profile));
    r.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();r.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(r.fixed_cost_table));r.latency=L;r.target_period_ps=1500;return r;
}
static pipelinev2::PipelineConfig config(const std::filesystem::path& root,const std::string& source,int L=3){
    pipelinev2::PipelineConfig c;c.source_name="interface.logic.cpp";c.source_text=source;c.top_function="top";c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=circt;c.arithmetic_request=request(root,L);c.pipeline.emplace();c.pipeline->register_stages=L-1;return c;
}
static void rejected(const Program& p,const std::string& code){
    bool failed=false;try{arithmetic::validate(p,"test_boundary");}catch(const rtlzz::ArithmeticError& e){CHECK(e.diagnostic.failure_code==code);CHECK(e.diagnostic.failure_stage=="test_boundary");failed=true;}CHECK(failed);
}

static Operand ref(const Program& p,NodeId id){Operand v;v.node=id;v.type=p.signal(id).type;return v;}
static Operand literal(int width,std::uint64_t value){Operand v;v.kind=OperandKind::Literal;v.type.width=width;v.constant.width=width;v.constant.limbs={value};return v;}
static NodeId raw(Program& p,int width,OperationKind kind,std::vector<Operand> args,OpCode op=OpCode::None){Signal s;s.id=p.signals.size();s.name="primitive_"+std::to_string(s.id);s.type.width=width;Operation d;d.kind=kind;d.type=s.type;d.op=op;d.operands=std::move(args);s.driver=d;p.signals.push_back(s);return s.id;}
static NodeId input(Program& p,int width,const std::string& name){Operand v;v.kind=OperandKind::Port;v.text=name;v.type.width=width;auto id=raw(p,width,OperationKind::PortRead,{v});p.signal(id).port_name=name;Port port;port.name=name;port.direction=PortDirection::Input;port.type.width=width;port.element_nodes={id};p.ports.push_back(port);p.inputs.push_back(name);return id;}
static Program selection(int index_width){
    Program p;auto a=input(p,8,"a"),b=input(p,8,"b"),index=input(p,index_width,"index"),sel=input(p,1,"sel");
    auto table=raw(p,8,OperationKind::Aggregate,{ref(p,a),ref(p,b),literal(8,1),literal(8,2)});p.signal(table).type.array_dims={4};p.signal(table).driver->type=p.signal(table).type;
    raw(p,8,OperationKind::Lookup,{ref(p,table),ref(p,index)});
    raw(p,8,OperationKind::Case,{ref(p,sel),ref(p,a),ref(p,sel),ref(p,b),literal(8,0)});
    return p;
}
static void primitiveContracts(){
    Program p;auto a=input(p,8,"a"),b=input(p,8,"b");
    auto carry=raw(p,9,OperationKind::AddCarry,{ref(p,a),ref(p,b),literal(1,1)});
    auto add=raw(p,8,OperationKind::Binary,{ref(p,a),ref(p,b)},OpCode::Add);
    auto slice=raw(p,4,OperationKind::Slice,{ref(p,a)});p.signal(slice).driver->hi=5;p.signal(slice).driver->lo=2;
    auto concat=raw(p,16,OperationKind::Concat,{ref(p,a),ref(p,b)});
    auto trunc=raw(p,4,OperationKind::Trunc,{ref(p,a)});p.signal(trunc).driver->to_width=4;
    arithmetic::validate(p,"test_boundary");auto costs=arithmetic::FixedCostProvider().group(p,{carry,add});CHECK(costs.delay==1&&costs.combinational==2);
    auto bad=p;bad.signal(carry).driver->operands[2]=literal(2,1);rejected(bad,"width_contract_violation");
    bad=p;bad.signal(carry).driver->operands[2]=literal(1,2);rejected(bad,"width_contract_violation");
    bad=p;bad.signal(slice).driver->hi=8;rejected(bad,"width_contract_violation");
    bad=p;bad.signal(concat).type.width=15;bad.signal(concat).driver->type.width=15;rejected(bad,"width_contract_violation");
    bad=p;bad.signal(trunc).driver->to_width=6;rejected(bad,"width_contract_violation");
    bad=p;bad.signal(add).driver->kind=OperationKind::Call;bad.signal(add).value.valid=true;bad.signal(add).value.constant=true;rejected(bad,"unsupported_backend_op");
    bad=p;bad.signal(a).driver->operands[0].text="missing";rejected(bad,"invalid_expansion_graph");
    bad=p;auto dynamic=raw(bad,1,OperationKind::DynamicBitSelect,{ref(bad,a),ref(bad,b)});rejected(bad,"template_ineligible");
    bad=p;raw(bad,1,OperationKind::DynamicBitSelect,{ref(bad,a),literal(3,7)});arithmetic::validate(bad,"test_boundary");CHECK(true);
    auto table=selection(2);arithmetic::validate(table,"test_boundary");auto lookup=table.signals.size()-2;CHECK(!arithmetic::FixedCostProvider().node(table,lookup).covered);
    auto normalized=arithmetic::normalizeSelections(table);arithmetic::validate(normalized,"test_boundary");
    unsigned muxes=0;std::vector<NodeId> all;for(const auto& s:normalized.signals){muxes+=s.driver->kind==OperationKind::Ite;all.push_back(s.id);CHECK(s.driver->kind!=OperationKind::Case&&s.driver->kind!=OperationKind::Lookup);}
    CHECK(muxes==5);auto total=arithmetic::FixedCostProvider().group(normalized,all);CHECK(total.delay==2&&total.combinational==5);
    bool rejected_index=false;try{arithmetic::normalizeSelections(selection(3));}catch(const rtlzz::ArithmeticError& e){rejected_index=e.diagnostic.failure_code=="template_ineligible";}CHECK(rejected_index);
    Program wide;auto x=input(wide,256,"x"),y=input(wide,256,"y");auto mul=raw(wide,512,OperationKind::Binary,{ref(wide,x),ref(wide,y)},OpCode::Mul);CHECK(!arithmetic::FixedCostProvider().node(wide,mul).covered);
    struct InvalidCost final:arithmetic::CostProvider {int mode=0;arithmetic::CostEstimate node(const Program&,NodeId id)const override{arithmetic::CostEstimate c;c.delay=mode==0?std::numeric_limits<double>::quiet_NaN():1;if(mode==1&&id%2)c.delay_unit="ps";return c;}};
    for(int mode:{0,1}){InvalidCost provider;provider.mode=mode;bool failed=false;try{provider.group(p,{carry,add});}catch(const rtlzz::ArithmeticError& e){failed=e.diagnostic.failure_code=="cost_uncovered";}CHECK(failed);}
}
int main(int argc,char** argv){
    primitiveContracts();
    CHECK(argc==3||argc==4);circt=argc==4;std::filesystem::path root=argv[1],out=argv[2];std::filesystem::create_directories(out);std::ofstream audit(out/"checks.txt");
    for(int w:{8,16,32,64,128,12,24,48,96}){
        auto width=std::to_string(w),result=std::to_string(2*w);
        auto c=config(root,"#include <fixint.hpp>\nInt<"+width+"> a,b,c;Int<"+result+"> y,z;bool iv,ov;\n#pragma input_port iv\n#pragma output_port ov\n#pragma input_port a\n#pragma input_port b\n#pragma input_port c\n#pragma output_port y\n#pragma output_port z\ninline Int<"+result+"> helper(Int<"+width+"> x,Int<"+width+"> y){return x*y;}\nvoid top(){y=helper(a,b);z=helper(a,c);}");
        c.top_function="mul"+width;auto pos=c.source_text->find("void top()");c.source_text->replace(pos,10,"void "+c.top_function+"()");
        c.pipeline->input_valid_name="iv";c.pipeline->output_valid_name="ov";c.pipeline->reset_enabled=true;
        auto reset=[&](unsigned value){std::vector<std::uint64_t> v((w+63)/64);v[0]=value;return v;};
        c.pipeline->input_reset_values={{"a",reset(5)},{"b",reset(7)},{"c",reset(11)},{"iv",{0}}};
        auto r=pipelinev2::compile(c);if(!r.ok())std::cerr<<w<<": "<<r.error<<"\n"<<r.error_debug_text;CHECK(r.ok());CHECK(r.pipeline_program);CHECK(r.beir_program->arithmetic_regions.size()==2);
        CHECK(r.pipeline_report->cost_model=="fixed-op-width-v1");
        auto json=llvm::json::parse(r.arithmetic_report);CHECK(static_cast<bool>(json));
        auto* report=json->getAsObject();CHECK(report);CHECK(report->getString("version")=="arithmetic-structure-report-v2");
        auto* pipeline=report->getObject("pipeline");CHECK(pipeline);
        CHECK(pipeline->getInteger("register_bits")==r.pipeline_report->register_bits);
        CHECK(pipeline->getInteger("register_count")==r.pipeline_report->register_count);
        for(const auto& item:std::vector<std::pair<std::string,std::uint64_t>>{{"y",35},{"z",55},{"ov",0}}) {
            const auto& reset=r.pipeline_report->output_reset_values.at(item.first);CHECK(!reset.empty());CHECK(reset[0]==item.second);for(std::size_t k=1;k<reset.size();++k)CHECK(reset[k]==0);
        }
        arithmetic::FixedCostProvider provider;
        for(const auto& region:r.beir_program->arithmetic_regions){CHECK(!region.internal_split_enabled);CHECK(region.template_version=="mul-original-v1");CHECK(region.template_parameters.at("result_width")==2*w);
            const auto cost=provider.group(r.pipeline_program->combinational,region.members);int bucket=1;while(bucket<w)bucket*=2;CHECK(cost.delay==std::max(1,bucket/2));CHECK(cost.combinational==std::max(1,bucket/2));
            for(auto id:region.members)CHECK(r.pipeline_program->stages[id]==r.pipeline_program->stages[region.outputs[0]]);
        }
        pipeline::validateProgram(*r.pipeline_program);auto repeated=beir::opt::optimizeProgram(*r.beir_program);regions::validate(repeated);CHECK(repeated.arithmetic_regions.size()==2);
        for(int side:{0,1}) {auto bad=*r.pipeline_program;(side?bad.rtl_program:bad.combinational).arithmetic_regions.clear();bool failed=false;try{pipeline::validateProgram(bad);}catch(const std::exception&){failed=true;}CHECK(failed);}
        auto bad=*r.pipeline_program;bad.rtl_program.arithmetic_regions[0].template_parameters["result_width"]++;bool failed=false;try{pipeline::validateProgram(bad);}catch(const std::exception&){failed=true;}CHECK(failed);
        auto p=r.pipeline_program->combinational;auto id=p.arithmetic_regions[0].members[0];
        auto corrupt=p;corrupt.signal(id).driver->kind=static_cast<OperationKind>(999);rejected(corrupt,"unsupported_backend_op");
        corrupt=p;corrupt.signal(id).driver->op=OpCode::LogicNot;rejected(corrupt,"unsupported_backend_op");
        corrupt=p;corrupt.signal(id).driver->operands.pop_back();rejected(corrupt,"invalid_expansion_graph");
        corrupt=p;corrupt.signal(id).driver->type.width++;rejected(corrupt,"width_contract_violation");
        corrupt=p;corrupt.signal(id).driver->operands[0].type.width++;rejected(corrupt,"width_contract_violation");
        corrupt=p;corrupt.signal(id).driver->operands[0].node=999999;rejected(corrupt,"invalid_expansion_graph");
        corrupt=p;corrupt.arithmetic_regions[1].members.push_back(id);rejected(corrupt,"invalid_expansion_graph");
        std::ofstream(out/("mul"+width+".sv"))<<r.output_text;std::ofstream(out/("mul"+width+".json"))<<r.arithmetic_report;audit<<"two atomic Mul regions W="<<w<<" full="<<2*w<<"; independent provider cost; metadata/shape/op/reference negatives passed\n";
    }
    std::string source="#include <arithmetic.hpp>\nInt<32> a,b,c;Int<37> y,z;Int<64> m;bool iv,ov;\n#pragma input_port iv\n#pragma output_port ov\n#pragma input_port a\n#pragma input_port b\n#pragma input_port c\n#pragma output_port y\n#pragma output_port z\n#pragma output_port m\ninline Int<37> wrap(Int<32>x,Int<32>w){return vularith::F32MulRNE(x,w);}\nvoid top(){y=wrap(a,b);z=wrap(a,c);m=a*b;}";
    auto fpconfig=config(root,source);fpconfig.pipeline->input_valid_name="iv";fpconfig.pipeline->output_valid_name="ov";fpconfig.pipeline->reset_enabled=true;
    fpconfig.pipeline->input_reset_values={{"a",{0x7f800000}},{"b",{0}},{"c",{0x3f800000}},{"iv",{0}}};
    auto fp=pipelinev2::compile(fpconfig);if(!fp.ok())std::cerr<<fp.error<<"\n"<<fp.arithmetic_report;CHECK(fp.ok());CHECK(fp.beir_program->arithmetic_regions.size()==3);
    CHECK(fp.pipeline_report->output_reset_values.at("y")==std::vector<std::uint64_t>{0x107fc00000ULL});
    CHECK(fp.pipeline_report->output_reset_values.at("z")==std::vector<std::uint64_t>{0x7f800000ULL});
    CHECK(fp.pipeline_report->output_reset_values.at("m")==std::vector<std::uint64_t>{0});
    // Report counters preserve the complete uint64 domain as JSON integers.
    auto huge=*fp.pipeline_report;huge.register_bits=std::numeric_limits<std::uint64_t>::max();
    auto hugeJson=llvm::json::parse(arithmetic::report(*fp.beir_program,request(root),&huge,fp.pipeline_program->stages,fp.output_text));
    CHECK(static_cast<bool>(hugeJson));auto* hugeValue=hugeJson->getAsObject()->getObject("pipeline")->get("register_bits");CHECK(hugeValue);
    CHECK(hugeValue->getAsUINT64()==std::numeric_limits<std::uint64_t>::max());
    for(const auto& r:fp.beir_program->arithmetic_regions){CHECK(!r.internal_split_enabled);auto level=fp.pipeline_program->stages[r.outputs[0]];for(auto id:r.members)CHECK(fp.pipeline_program->stages[id]==level);}
    auto swapped=*fp.pipeline_program;std::swap(swapped.rtl_program.arithmetic_regions[0].inputs[0],swapped.rtl_program.arithmetic_regions[0].inputs[1]);
    bool swap_rejected=false;try{pipeline::validateProgram(swapped);}catch(const std::exception&){swap_rejected=true;}CHECK(swap_rejected);
    struct BoundaryProvider final:arithmetic::CostProvider {
        mutable bool queried=false;
        arithmetic::CostEstimate node(const Program&,NodeId)const override{return {};}
        arithmetic::CostEstimate estimateGroup(const Program&,const std::vector<NodeId>&)const override{queried=true;return {};}
    } guard;
    bool guard_rejected=false;try{guard.group(*fp.beir_program,{fp.beir_program->arithmetic_regions[0].members[0]});}catch(const rtlzz::ArithmeticError&){guard_rejected=true;}
    CHECK(guard_rejected);CHECK(!guard.queried);guard.group(*fp.beir_program,fp.beir_program->arithmetic_regions[0].members);CHECK(guard.queried);
    bool partial_rejected=false;try{arithmetic::FixedCostProvider().group(*fp.beir_program,{fp.beir_program->arithmetic_regions[0].members[0]});}catch(const rtlzz::ArithmeticError& e){partial_rejected=e.diagnostic.failure_code=="invalid_expansion_graph";}CHECK(partial_rejected);
    for(const auto& replacement:std::vector<std::pair<std::string,std::string>>{{"z=wrap(a,c)","z=wrap(a,b)"},{"z=wrap(a,c)","z=wrap(Int<32>(y),c)"},{"y=wrap(a,b)","y=wrap(Int<32>(0x3f800000),Int<32>(0x40000000))"}}) {
        auto variant=fpconfig;auto pos=variant.source_text->find(replacement.first);variant.source_text->replace(pos,replacement.first.size(),replacement.second);
        auto r=pipelinev2::compile(variant);if(!r.ok())std::cerr<<r.error<<"\n"<<r.arithmetic_report;CHECK(r.ok());CHECK(r.beir_program->arithmetic_regions.size()>=2);
        for(const auto& region:r.beir_program->arithmetic_regions){CHECK(!region.internal_split_enabled);for(auto id:region.members)CHECK(r.pipeline_program->stages[id]==r.pipeline_program->stages[region.outputs[0]]);}
    }
    std::ofstream(out/"mixed_fp.sv")<<fp.output_text;std::ofstream(out/"mixed_fp.json")<<fp.arithmetic_report;
    auto c=config(root,source);for(int failure=0;failure<7;++failure){auto bad=c;
        if(failure==0)bad.arithmetic_request->semantic_profile="fixed_point";
        if(failure==1)bad.arithmetic_request->candidate_policy="mul_half";
        if(failure==2)bad.arithmetic_request->initiation_interval=2;
        if(failure==3)bad.arithmetic_request->latency=4;
        if(failure==4)bad.arithmetic_request->target_platform="sky130hd";
        if(failure==5)bad.arithmetic_request->cost_mode="backend-model";
        if(failure==6)bad.arithmetic_request->template_parameters["typo"]=8;
        auto r=pipelinev2::compile(bad);CHECK(!r.ok());CHECK(r.arithmetic_diagnostic);CHECK(!r.arithmetic_report.empty());audit<<"request reject "<<failure<<" "<<r.arithmetic_diagnostic->failure_code<<"\n";
    }
    // Public CompileOptions and direct PipelineV2 consume the same request.
    rtlzz::CompileOptions api;api.source_codelines={source};api.source_name="interface.logic.cpp";api.top_function="top";api.vullib_dir=(root/"vullib").string();api.arithmetic_request=request(root);api.register_stages=2;api.use_circt=circt;api.input_valid_name="iv";api.output_valid_name="ov";api.reset_enabled=true;api.input_reset_values=fpconfig.pipeline->input_reset_values;
    auto pub=rtlzz::compileToRtl(api);if(!pub.ok())std::cerr<<pub.error;CHECK(pub.ok());if(!circt)CHECK(pub.arithmetic_report==fp.arithmetic_report);
    // Combinational requests also receive costs/validation, without requiring a reset/valid channel.
    auto combo=config(root,source,1);combo.pipeline.reset();combo.source_text->replace(combo.source_text->find("void top()"),10,"void fp_comb()");combo.top_function="fp_comb";combo.source_text->replace(combo.source_text->find("m=a*b;"),6,"m=a*b;ov=false;");auto comb=pipelinev2::compile(combo);CHECK(comb.ok());CHECK(comb.pipeline_report);CHECK(comb.pipeline_report->register_bits==0);
    std::ofstream(out/"mixed_fp_comb.sv")<<comb.output_text;
    std::cout<<"PASS arithmetic interfaces checks="<<checks<<"\n";
}
