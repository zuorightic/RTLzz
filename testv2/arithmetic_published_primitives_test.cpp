#include "backend/arithmetic_primitives.hpp"
#include "backend/arithmetic_selection.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <numeric>
#include <set>

using namespace pred;
using namespace beir;
using namespace beir::arithmetic;
using Object=llvm::json::Object;
static std::string read(const std::filesystem::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
static void check(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
static NodeId add(Program& p,int width,OperationKind kind,std::vector<Operand> operands={},OpCode op=OpCode::None){
    Signal s;s.id=p.signals.size();s.name="n"+std::to_string(s.id);s.type.width=width;Operation d;d.type=s.type;d.kind=kind;d.op=op;d.operands=std::move(operands);s.driver=d;p.signals.push_back(s);return s.id;
}
static Operand ref(const Program& p,NodeId id){Operand a;a.kind=OperandKind::Symbol;a.node=id;a.type=p.signal(id).type;return a;}
static Operand literal(int w,const std::string& value){Operand a;a.kind=OperandKind::Literal;a.type.width=w;a.constant.width=w;a.constant.limbs.assign((w+63)/64,value=="ones"?UINT64_MAX:value=="one"?1:0);return a;}
static NodeId input(Program& p,int w){Operand a;a.kind=OperandKind::Port;a.type.width=w;a.text="in"+std::to_string(p.signals.size());auto id=add(p,w,OperationKind::PortRead,{a});
    Port port;port.name=a.text;port.direction=PortDirection::Input;port.type.width=w;port.element_nodes={id};p.ports.push_back(port);p.inputs.push_back(port.name);return id;
}
static NodeId output(Program& p,NodeId id){auto result=add(p,p.signal(id).type.width,OperationKind::Assign,{ref(p,id)});Port port;port.name="out"+std::to_string(result);port.direction=PortDirection::Output;port.type=p.signal(id).type;port.element_nodes={result};p.ports.push_back(port);p.outputs.push_back(port.name);return result;}
struct Fixture {Program p;std::vector<NodeId> paid;std::vector<int> placement;int last=0;};
static Fixture fixture(const std::string& name,int lhs,int rhs,int width,unsigned count=1){
    Fixture f;f.p.function_name="PublishedPrimitiveFixture";
    for(unsigned i=0;i<count;++i){auto a=input(f.p,lhs);NodeId b=kInvalidNodeId,id=kInvalidNodeId;
        if(rhs)b=input(f.p,rhs);
        if(name.rfind("mux",0)==0){auto select=input(f.p,1);if(name=="mux")b=input(f.p,lhs);
            auto other=name=="mux"?ref(f.p,b):literal(lhs,name.substr(4));id=add(f.p,width,OperationKind::Ite,{ref(f.p,select),ref(f.p,a),other});
        }else if(name=="eq_zero")id=add(f.p,width,OperationKind::Unary,{ref(f.p,a)},OpCode::LogicNot);
        else if(name=="addcarry"){auto carry=input(f.p,1);id=add(f.p,width,OperationKind::AddCarry,{ref(f.p,a),ref(f.p,b),ref(f.p,carry)});}
        else {auto op=name=="mul"?OpCode::Mul:name=="add"?OpCode::Add:name=="div"?OpCode::Div:name=="mod"?OpCode::Mod:OpCode::BitOr;
            if(name=="div"||name=="mod"){
                auto equal=add(f.p,1,OperationKind::Unary,{ref(f.p,b)},OpCode::LogicNot);
                b=add(f.p,rhs,OperationKind::Ite,{ref(f.p,equal),literal(rhs,"one"),ref(f.p,b)});f.last=1;
            }
            id=add(f.p,width,OperationKind::Binary,{ref(f.p,a),ref(f.p,b)},op);
        }
        f.paid.push_back(id);output(f.p,id);
    }
    f.placement.assign(f.p.signals.size(),0);
    if(f.last)for(auto id:f.paid){f.placement[id]=1;f.placement[id+1]=1;}
    return f;
}
static Object value(const CostEstimate& cost){Object fs;for(auto [name,v]:cost.physical_features)fs[name]=v;return Object{{"operation",cost.physical_operation},{"delay_ps",cost.delay},{"comb_area_um2",cost.combinational},{"features",std::move(fs)}};}
int main(int argc,char** argv){try {
    check(argc==4,"root, model, output required");std::filesystem::path root=argv[1],model=argv[2],out=argv[3];std::filesystem::create_directories(out);
    rtlzz::ArithmeticRequest request;request.target_platform="asap7";request.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();request.platform_profile_sha256=rtlzz::arithmeticHash(read(request.platform_profile));
    request.cost_mode="backend-model";request.model_bundle=model.string();request.model_bundle_sha256=rtlzz::arithmeticHash(read(model));request.target_period_ps=1500;request.candidate_policy="mul-candidates-v1";request.latency=2;
    rtlzz::validateArithmeticRequest(request,1);
    auto parsed=llvm::json::parse(read(model));check(bool(parsed),"model JSON");const auto& operations=*parsed->getAsObject()->getObject("stages")->getObject("grt")->getObject("operations");
    llvm::json::Array results,rejections;std::set<std::string> exercised;
    pipelinev2::PipelineConfig fp;fp.source_name="published-fp.logic.cpp";fp.source_text="#include <fixint.hpp>\n#include <arithmetic.hpp>\nInt<32>a,b;Int<37>y;\n#pragma input_port a\n#pragma input_port b\n#pragma output_port y\nvoid top(){y=vularith::F32MulRNE(a,b);}\n";
    fp.top_function="top";fp.clang_args={"-std=c++20","-I"+(root/"vullib").string()};fp.use_circt=false;auto compiled=pipelinev2::compile(fp);check(compiled.ok()&&compiled.beir_program.has_value(),"public binary32 source compile");
    for(const auto& entry:operations){auto name=entry.first.str();const auto& domains=*entry.second.getAsObject()->getArray("joint_domain");
        for(const auto& condition:domains){const auto& d=*condition.getAsObject();int lhs=*d.getInteger("lhs_width"),rhs=*d.getInteger("rhs_width"),width=*d.getInteger("result_width");
            for(unsigned count:name=="mux"?std::vector<unsigned>{2,3,4}:std::vector<unsigned>{1}){
                request.target_period_ps=*d.getNumber("sdc_period_ps");Fixture f;
                if(name=="fp32"){f.p=*compiled.beir_program;f.placement.assign(f.p.signals.size(),0);for(const auto& r:f.p.arithmetic_regions)if(r.template_version=="softfloat3d-bitvector-mul-v1")f.paid={r.outputs.front()};check(!f.paid.empty(),"real FP atom");}
                else f=fixture(name,lhs,rhs,width,count);
                auto provider=makeCostProvider(f.p,request);check(provider->requiresStageContext(),"factory did not select published consumer");
                const auto cut=stageCostQuery(f.p,f.placement,f.last,f.last);
                for(auto id:f.paid){CostEstimate c;try{c=provider->nodeInStage(f.p,id,cut);}catch(const rtlzz::ArithmeticError& e){std::cerr<<name<<" lhs="<<lhs<<" rhs="<<rhs<<" result="<<width<<" target="<<request.target_period_ps<<" node="<<id<<" captures=";for(auto capture:cut.captures)std::cerr<<capture.value<<':'<<capture.banks<<',';std::cerr<<'\n';throw;}
                    check(c.covered&&!c.deferred_to_stage&&c.delay>0&&c.combinational>0,"qualified component price");auto row=value(c);row["fixture_operation"]=name;row["parallel_count"]=count;row["graph_sha256"]=cut.graph_sha256;row["cut_sha256"]=cut.cut_sha256;results.push_back(std::move(row));exercised.insert(c.physical_operation);}
            }
        }
    }
    auto refused=[&](const std::string& label,auto action){bool failed=false;try{action();}catch(const rtlzz::ArithmeticError& e){failed=e.diagnostic.failure_code=="cost_uncovered";rejections.push_back(Object{{"test",label},{"reason",e.diagnostic.failure_reason}});}check(failed,label.c_str());};
    request.target_period_ps=1500;auto f=fixture("mux",8,0,8,4);f.last=1;f.placement.assign(f.p.signals.size(),0);for(const auto& port:f.p.ports)if(port.direction==PortDirection::Output)for(auto id:port.element_nodes)f.placement[id]=1;
    auto provider=makeCostProvider(f.p,request);auto before=stageCostQuery(f.p,f.placement,0,1);auto first=provider->nodeInStage(f.p,f.paid[0],before);
    auto moved=f.placement;moved[f.paid[2]]=moved[f.paid[3]]=1;auto after=stageCostQuery(f.p,moved,0,1);auto second=provider->nodeInStage(f.p,f.paid[0],after);
    check(first.physical_features.at("context_dynamic_mux_count")==4&&second.physical_features.at("context_dynamic_mux_count")==2&&before.cut_sha256!=after.cut_sha256,"actual placement refresh");
    auto stale=before;stale.placement=moved;refused("stale-stage-cut",[&]{provider->nodeInStage(f.p,f.paid[0],stale);});
    auto forged=before;forged.members.pop_back();refused("forged-stage-members",[&]{provider->nodeInStage(f.p,f.paid[0],forged);});
    auto shared=f;shared.p.signal(shared.paid[1]).driver->operands[0]=shared.p.signal(shared.paid[0]).driver->operands[0];
    refused("shared-selector",[&]{auto c=makeCostProvider(shared.p,request);c->nodeInStage(shared.p,shared.paid[0],stageCostQuery(shared.p,shared.placement,0,1));});
    auto single=fixture("mux",8,0,8,1);refused("one-mux-stage",[&]{auto c=makeCostProvider(single.p,request);c->nodeInStage(single.p,single.paid[0],stageCostQuery(single.p,single.placement,0,0));});
    auto mixed=f;auto ma=input(mixed.p,8),mb=input(mixed.p,8);auto extra=add(mixed.p,8,OperationKind::Binary,{ref(mixed.p,ma),ref(mixed.p,mb)},OpCode::BitOr);output(mixed.p,extra);
    mixed.placement.resize(mixed.p.signals.size(),0);mixed.placement.back()=1;
    refused("mixed-stage-context",[&]{auto c=makeCostProvider(mixed.p,request);c->nodeInStage(mixed.p,mixed.paid[0],stageCostQuery(mixed.p,mixed.placement,0,1));});
    auto controls=fixture("mux",1,0,1,2);auto selector=controls.p.signal(controls.paid[0]).driver->operands[0].node;
    refused("arithmetic-disguised-as-valid",[&]{stageCostQuery(controls.p,controls.placement,0,0,{selector,controls.paid[0]+1});});
    auto masked=*compiled.beir_program;const auto& atom=masked.arithmetic_regions.front();auto fp_input=atom.inputs.front().node;
    masked.signal(fp_input).value.valid=true;masked.signal(fp_input).value.width=32;masked.signal(fp_input).value.known_one={1};
    refused("binary32-known-input-pattern",[&]{auto c=makeCostProvider(masked,request);c->nodeInStage(masked,atom.outputs.front(),stageCostQuery(masked,std::vector<int>(masked.signals.size(),0),0,0));});
    auto bank=f;for(const auto& port:f.p.ports)if(port.direction==PortDirection::Output){auto extra=port;extra.name+="extra";bank.p.ports.push_back(extra);}
    auto bank_stages=bank.placement;for(int& at:bank_stages)at=0;
    refused("extra-capture-bank",[&]{auto c=makeCostProvider(bank.p,request);c->nodeInStage(bank.p,bank.paid[0],stageCostQuery(bank.p,bank_stages,0,0));});
    auto bad=request;bad.target_period_ps=1234;refused("unregistered-target",[&]{auto c=makeCostProvider(f.p,bad);c->nodeInStage(f.p,f.paid[0],before);});
    bad=request;bad.platform_profile_sha256="wrong";refused("wrong-profile",[&]{makeCostProvider(f.p,bad);});
    bad=request;bad.model_bundle_sha256="wrong";refused("wrong-model-hash",[&]{makeCostProvider(f.p,bad);});
    refused("missing-state",[&]{provider->validateStateProtocol(true,true,true);});
    refused("missing-boundary",[&]{provider->dataBudget(true);});
    refused("missing-stage-model",[&]{provider->stageDelay(1,4,32,{});});
    check(exercised.size()==12,"all published operations exercised");
    // Public D compilation must retain all three ordered attempts, and must
    // report missing qualification rather than substitute fixed/table prices.
    pipelinev2::PipelineConfig source;source.source_name="published-D.logic.cpp";source.source_text="#include <fixint.hpp>\nInt<8>a,b;Int<16>y;bool iv,ov;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port ov\nvoid top(){y=a*b;}\n";
    source.top_function="top";source.clang_args=fp.clang_args;source.use_circt=false;source.arithmetic_request=request;source.pipeline.emplace();source.pipeline->register_stages=1;source.pipeline->reset_enabled=true;source.pipeline->input_valid_name="iv";source.pipeline->output_valid_name="ov";
    source.pipeline->input_reset_values={{"a",{0}},{"b",{0}},{"iv",{0}}};auto d=pipelinev2::compile(source);
    check(!d.ok()&&d.arithmetic_report.find("published_primitive_model_missing_state_model")!=std::string::npos,"D must reject missing state qualification");
    auto diagnostic=llvm::json::parse(d.arithmetic_report);check(bool(diagnostic),"D diagnostic JSON");auto selection=diagnostic->getAsObject()->getObject("selection");check(selection&&selection->getArray("candidates")->size()==3,"retain all original/half/quarter D attempts");
    std::ofstream(out/"D-diagnostic.json")<<d.arithmetic_report;
    auto control=source;
    control.source_name="published-stage-binding.logic.cpp";
    control.source_text="#include <fixint.hpp>\nInt<8>a,b,c,d,y,z;bool sa,sb,iv,ov;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port c\n#pragma input_port d\n#pragma input_port sa\n#pragma input_port sb\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port z\n#pragma output_port ov\nvoid top(){y=sa?a:b;z=sb?c:d;}\n";
    control.arithmetic_request->cost_mode="fixed";control.arithmetic_request->model_bundle.clear();control.arithmetic_request->model_bundle_sha256.clear();
    control.arithmetic_request->fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();
    control.arithmetic_request->fixed_cost_table_sha256=rtlzz::arithmeticHash(read(control.arithmetic_request->fixed_cost_table));
    control.pipeline->input_reset_values={{"a",{0}},{"b",{0}},{"c",{0}},{"d",{0}},{"sa",{0}},{"sb",{0}},{"iv",{0}}};
    auto placed=pipelinev2::compile(control);check(placed.ok()&&placed.pipeline_program.has_value(),"real C pipeline for component binding");
    const auto& actual=*placed.pipeline_program;auto bound=makeCostProvider(actual.combinational,request);std::vector<NodeId> valid(2,kInvalidNodeId);
    for(const auto& port:actual.combinational.ports)if(port.name=="iv"||port.name=="ov")valid[port.name=="iv"?0:1]=port.element_nodes.front();
    auto component=bound->componentReport(actual.combinational,actual.stages,actual.options.register_stages,valid);
    auto actual_report=llvm::json::parse(component);check(bool(actual_report),"actual component report");
    check(actual_report->getAsObject()->getInteger("covered_components")==2&&actual_report->getAsObject()->getInteger("uncovered_components")==0,"prices must bind actual PipelineV2 stage/capture context");
    std::ofstream(out/"actual-C-placement-components.json")<<component;
    std::ofstream(out/"actual-C-placement.sv")<<placed.output_text;
    std::ofstream(out/"queries.json")<<llvm::formatv("{0:2}",llvm::json::Value(Object{{"version","published-primitive-native-queries-v1"},{"model_sha256",request.model_bundle_sha256},{"PPA_calls",0},{"operations",static_cast<std::uint64_t>(exercised.size())},{"rows",std::move(results)},{"rejections",std::move(rejections)},{"placement_refresh_checked",true},{"full_D_ready",false}})).str();
    std::cout<<"PASS production published primitive factory, stage queries, placement refresh and D coverage diagnostics\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
