#include "rtlzz.hpp"
#include "arithmetic_request.hpp"
#include "pipelinev2/PipelineV2.h"
#include "backend/arithmetic_model.hpp"
#include "backend/pipeline.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <cmath>
#include <set>
using namespace pred;using namespace beir;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<__LINE__<<": "<<#x<<"\n";return 1;}}while(false)
static std::string read(const std::filesystem::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
static Operand ref(const Program& p,NodeId id){Operand a;a.node=id;a.type=p.signal(id).type;return a;}
static NodeId input(Program& p,int width,const std::string& name){Signal s;s.id=p.signals.size();s.name=name;s.type.width=width;Operation d;d.kind=OperationKind::PortRead;d.type=s.type;Operand a;a.kind=OperandKind::Port;a.type=s.type;a.text=name;d.operands={a};s.driver=d;p.signals.push_back(s);return s.id;}
int main(int argc,char**argv){
    CHECK(argc==4||argc==5);const bool expect_not_ready=argc==5;CHECK(!expect_not_ready||std::string(argv[4])=="--expect-not-ready");
    std::filesystem::path root=argv[1],models=argv[2],out=argv[3];std::filesystem::create_directories(out);
    llvm::json::Array results;
    for(const std::string platform:{"asap7","nangate45","sky130hd","gf180"}){
        rtlzz::ArithmeticRequest request;request.target_platform=platform;request.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms"/(platform+".json")).string();request.platform_profile_sha256=rtlzz::arithmeticHash(read(request.platform_profile));
        request.cost_mode="backend-model";request.model_bundle=(models/(platform+".json")).string();request.model_bundle_sha256=rtlzz::arithmeticHash(read(request.model_bundle));request.candidate_policy="mul-candidates-v1";request.latency=3;
        auto json=llvm::json::parse(read(request.model_bundle));CHECK(static_cast<bool>(json));auto* bundle=json->getAsObject();CHECK(bundle);request.target_period_ps=*bundle->getNumber("mapping_abc_target_ps");
        if(expect_not_ready){
            CHECK(bundle->getString("quality_status")!="passed");bool refused=false;std::string code;
            try{rtlzz::validateArithmeticRequest(request,2);}catch(const rtlzz::ArithmeticError& e){code=e.diagnostic.failure_code;refused=code=="cost_uncovered";}
            CHECK(refused);results.push_back(llvm::json::Object{{"platform",platform},{"model_sha256",request.model_bundle_sha256},{"quality_status",bundle->getString("quality_status")->str()},{"refused",true},{"failure_code",code}});continue;
        }
        rtlzz::validateArithmeticRequest(request,2);
        for(int w:{2,4,8,16,32}){
            Program p;auto a=input(p,w,"a"),b=input(p,w,"b");Signal s;s.id=p.signals.size();s.name="product";s.type.width=2*w;Operation d;d.kind=OperationKind::Binary;d.op=OpCode::Mul;d.type=s.type;d.operands={ref(p,a),ref(p,b)};s.driver=d;p.signals.push_back(s);
            arithmetic::ModelCostProvider provider(p,request);auto estimate=provider.node(p,s.id);
            CHECK(estimate.covered&&estimate.delay>=0&&estimate.combinational>0);CHECK(estimate.delay_unit=="ps"&&estimate.area_unit=="um2");
            results.push_back(llvm::json::Object{{"platform",platform},{"width",w},{"delay_ps",estimate.delay},{"comb_area_um2",estimate.combinational},{"model_id",provider.modelIdentity()}});
            auto masked=p;masked.signal(a).value.valid=true;masked.signal(a).value.width=w;masked.signal(a).value.known_zero={1};bool refused=false;
            try{arithmetic::ModelCostProvider cost(masked,request);cost.node(masked,s.id);}catch(const rtlzz::ArithmeticError& e){refused=e.diagnostic.failure_code=="cost_uncovered";}CHECK(refused);
            auto constant=p;auto& value=constant.signal(s.id).driver->operands[1];value.kind=OperandKind::Literal;value.constant.width=w;value.constant.limbs={(std::uint64_t(1)<<w)-1};refused=false;
            try{arithmetic::ModelCostProvider cost(constant,request);cost.node(constant,s.id);}catch(const rtlzz::ArithmeticError& e){refused=e.diagnostic.failure_code=="cost_uncovered";}CHECK(refused);
            auto correlated=p;correlated.signal(s.id).driver->operands[1]=ref(correlated,a);refused=false;
            try{arithmetic::ModelCostProvider cost(correlated,request);cost.node(correlated,s.id);}catch(const rtlzz::ArithmeticError& e){refused=e.diagnostic.failure_code=="cost_uncovered";}CHECK(refused);
            auto ones=p;ones.signal(a).value.valid=true;ones.signal(a).value.width=w;ones.signal(a).value.known_one={1};refused=false;
            try{arithmetic::ModelCostProvider cost(ones,request);cost.node(ones,s.id);}catch(const rtlzz::ArithmeticError& e){refused=e.diagnostic.failure_code=="cost_uncovered";}CHECK(refused);
            auto carry=p;auto cin=input(carry,1,"cin");auto& sum=carry.signal(s.id);sum.type.width=w+1;sum.driver->type=sum.type;sum.driver->kind=OperationKind::AddCarry;sum.driver->op=OpCode::None;sum.driver->operands.push_back(ref(carry,cin));
            {arithmetic::ModelCostProvider cost(carry,request);CHECK(cost.node(carry,s.id).delay>0);}
            auto literal_carry=carry;auto& operand=literal_carry.signal(s.id).driver->operands[2];operand.kind=OperandKind::Literal;operand.constant.width=1;operand.constant.limbs={1};refused=false;
            try{arithmetic::ModelCostProvider cost(literal_carry,request);cost.node(literal_carry,s.id);}catch(const rtlzz::ArithmeticError& e){refused=e.diagnostic.failure_code=="cost_uncovered";}CHECK(refused);
            auto shared_carry=carry;auto& bit=*shared_carry.signal(cin).driver;bit.kind=OperationKind::BitSelect;bit.bit=0;bit.operands={ref(shared_carry,a)};refused=false;
            try{arithmetic::ModelCostProvider cost(shared_carry,request);cost.node(shared_carry,s.id);}catch(const rtlzz::ArithmeticError& e){refused=e.diagnostic.failure_code=="cost_uncovered";}CHECK(refused);
            if(w==8){
                auto trees=p;auto c0=input(trees,w,"c0"),d0=input(trees,w,"d0");auto second=trees.signal(s.id);second.id=trees.signals.size();second.name="second_product";second.driver->operands={ref(trees,c0),ref(trees,d0)};trees.signals.push_back(second);
                arithmetic::ModelCostProvider cost(trees,request);CHECK(cost.stageShapeCovered({s.id}));CHECK(!cost.stageShapeCovered({s.id,second.id}));
                Signal add;add.id=trees.signals.size();add.name="root_sum";add.type.width=2*w;Operation op;op.type=add.type;op.kind=OperationKind::Binary;op.op=OpCode::Add;op.operands={ref(trees,s.id),ref(trees,second.id)};add.driver=op;trees.signals.push_back(add);
                arithmetic::ModelCostProvider connected(trees,request);CHECK(connected.stageShapeCovered({s.id,second.id,add.id}));
            }
        }
        pipelinev2::PipelineConfig c;c.source_name="model-test.logic.cpp";c.source_text="#include <fixint.hpp>\nInt<8> a,b;Int<16> y;bool iv,ov;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port ov\nvoid top(){y=a*b;}\n";
        c.top_function="top";c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.arithmetic_request=request;c.pipeline.emplace();c.pipeline->register_stages=2;c.pipeline->reset_enabled=true;c.pipeline->input_valid_name="iv";c.pipeline->output_valid_name="ov";
        c.pipeline->input_reset_values={{"a",{0}},{"b",{0}},{"iv",{0}}};
        auto compiled=pipelinev2::compile(c);if(!compiled.ok())std::cerr<<compiled.error<<"\n"<<compiled.arithmetic_report<<"\n";CHECK(compiled.ok());CHECK(compiled.pipeline_program);pipeline::validateProgram(*compiled.pipeline_program);
        CHECK(compiled.pipeline_report->model_feasible);CHECK(compiled.pipeline_report->predicted_total_area_um2>0);CHECK(compiled.pipeline_report->final_delay<=compiled.pipeline_report->allowed_delay);
        std::ofstream(out/(platform+".report.json"))<<compiled.arithmetic_report;std::ofstream(out/(platform+".sv"))<<compiled.output_text;
        // Enumerate only three placements of this fixed atomic Mul. These
        // are independently evaluated positions, never a fabricated search trace.
        llvm::json::Array placements;unsigned feasible_positions=0;
        const auto& piped=*compiled.pipeline_program;const auto& graph=piped.combinational;
        for(int position=0;position<3;++position){
            auto proposed=piped.stages;std::set<NodeId> fixed_inputs,fixed_outputs;
            for(const auto& port:graph.ports)for(auto node:port.element_nodes)(port.direction==PortDirection::Input?fixed_inputs:fixed_outputs).insert(node);
            for(const auto& region:graph.arithmetic_regions)for(auto node:region.members)proposed[node]=position;
            for(std::size_t pass=0;pass<graph.signals.size();++pass)for(const auto& signal:graph.signals)if(!fixed_inputs.count(signal.id)&&!fixed_outputs.count(signal.id))
                for(const auto& operand:signal.driver->operands)if(operand.kind==OperandKind::Symbol)proposed[signal.id]=std::max(proposed[signal.id],proposed[operand.node]);
            const auto evaluated=pipeline::evaluateSchedule(graph,piped.options,proposed);
            bool feasible=std::isfinite(evaluated.final_delay)&&evaluated.model_feasible;
            for(std::size_t k=0;k<evaluated.stage_delays.size();++k)feasible&=evaluated.stage_delays[k]<=piped.report.allowed_stage_delays[k];
            feasible_positions+=feasible;llvm::json::Array levels,delays;
            for(auto v:proposed)levels.push_back(v);for(auto v:evaluated.stage_delays)delays.push_back(v);
            llvm::json::Object row{{"position",position},{"stages",std::move(levels)},{"feasible",feasible},{"stage_delays_ps",std::move(delays)},
                {"register_bits",evaluated.register_bits},{"register_count",static_cast<std::uint64_t>(evaluated.register_count)},
                {"model_state_bits",evaluated.model_state_bits},{"model_state_signals",static_cast<std::uint64_t>(evaluated.model_state_signals)}};
            if(std::isfinite(evaluated.final_delay)){row["total_area_um2"]=evaluated.predicted_total_area_um2;row["comb_area_um2"]=evaluated.predicted_comb_area_um2;row["state_area_um2"]=evaluated.predicted_state_area_um2;}
            placements.push_back(std::move(row));
        }
        CHECK(feasible_positions>=2);std::ofstream(out/(platform+".positions.json"))<<llvm::formatv("{0:2}",llvm::json::Value(std::move(placements))).str()<<"\n";
        auto fixed=c;fixed.arithmetic_request->cost_mode="fixed";fixed.arithmetic_request->model_bundle.clear();fixed.arithmetic_request->model_bundle_sha256.clear();fixed.arithmetic_request->fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.arithmetic_request->fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.arithmetic_request->fixed_cost_table));
        auto control=pipelinev2::compile(fixed);if(!control.ok())std::cerr<<control.error;CHECK(control.ok());std::ofstream(out/(platform+".C.report.json"))<<control.arithmetic_report;std::ofstream(out/(platform+".C.sv"))<<control.output_text;
        auto second=c;second.arithmetic_request->target_period_ps*=1.5;auto changed=pipelinev2::compile(second);std::ofstream(out/(platform+".context2.report.json"))<<changed.arithmetic_report;
        if(changed.ok()){CHECK(changed.pipeline_report->input_data_budget_ps!=compiled.pipeline_report->input_data_budget_ps);CHECK(changed.pipeline_report->model_feasible);}
        else {CHECK(changed.arithmetic_diagnostic);CHECK(changed.arithmetic_diagnostic->failure_code=="cost_uncovered");}
        // A correctly hashed bundle of another platform must never fall back.
        auto mismatch=request;mismatch.target_platform=platform=="asap7"?"nangate45":"asap7";bool refused=false;
        try{rtlzz::validateArithmeticRequest(mismatch,2);}catch(const rtlzz::ArithmeticError&){refused=true;}CHECK(refused);
        auto tight=c;tight.arithmetic_request->target_period_ps=1;auto rejected=pipelinev2::compile(tight);CHECK(!rejected.ok());CHECK(!rejected.arithmetic_report.empty());
    }
    std::ofstream(out/(expect_not_ready?"not-ready.json":"parity.json"))<<llvm::formatv("{0:2}",llvm::json::Value(std::move(results))).str()<<"\n";
    std::cout<<"Arithmetic model native checks: "<<checks<<"\n";return 0;
}
