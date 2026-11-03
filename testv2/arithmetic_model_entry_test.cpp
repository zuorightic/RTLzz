#include "rtlzz.hpp"
#include "arithmetic_request.hpp"
#include "pipelinev2/PipelineV2.h"
#include "backend/pipeline.hpp"
#include <llvm/Support/JSON.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace pred;
namespace fs=std::filesystem;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<__LINE__<<": "<<#x<<"\n";return 1;}}while(false)
static std::string read(const fs::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char**argv){
    CHECK(argc==4);fs::path root=argv[1],models=argv[2],out=argv[3];fs::create_directories(out);
    for(const std::string platform:{"asap7","nangate45","sky130hd","gf180"}){
        rtlzz::ArithmeticRequest r;r.target_platform=platform;r.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms"/(platform+".json")).string();r.platform_profile_sha256=rtlzz::arithmeticHash(read(r.platform_profile));
        r.cost_mode="backend-model";r.model_bundle=(models/(platform+".json")).string();r.model_bundle_sha256=rtlzz::arithmeticHash(read(r.model_bundle));r.candidate_policy="mul-candidates-v1";r.latency=3;
        auto b=llvm::json::parse(read(r.model_bundle));CHECK(static_cast<bool>(b));double reference=*b->getAsObject()->getNumber("mapping_abc_target_ps");
        for(int context=0;context<2;++context){
            r.target_period_ps=reference*(context?1.5:1.0);
            pipelinev2::PipelineConfig c;c.source_name="model-carry-entry.logic.cpp";
            c.source_text="#include <fixint.hpp>\nInt<8> a,b;bool carry,iv,ov;Int<9> y;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port carry\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port ov\nvoid carryTop(){y=AddCarry(a,b,carry);}\n";
            c.top_function="carryTop";c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.arithmetic_request=r;c.pipeline.emplace();c.pipeline->register_stages=2;c.pipeline->reset_enabled=true;c.pipeline->input_valid_name="iv";c.pipeline->output_valid_name="ov";
            c.pipeline->input_reset_values={{"a",{0}},{"b",{0}},{"carry",{0}},{"iv",{0}}};
            auto result=pipelinev2::compile(c);if(!result.ok())std::cerr<<result.error<<"\n"<<result.arithmetic_report;CHECK(result.ok());CHECK(result.pipeline_program);pipeline::validateProgram(*result.pipeline_program);
            auto report=llvm::json::parse(result.arithmetic_report);CHECK(static_cast<bool>(report));auto* selection=report->getAsObject()->getObject("selection");CHECK(selection);CHECK(selection->getBoolean("formal_D_selection")==true);CHECK(selection->getInteger("evaluated_structures")==1);CHECK(selection->getInteger("candidate_PPA_calls")==0);
            const auto stem=platform+".carry.context"+std::to_string(context);
            std::ofstream(out/(stem+".json"))<<result.arithmetic_report;std::ofstream(out/(stem+".sv"))<<result.output_text;
            auto fixed=c;fixed.arithmetic_request->cost_mode="fixed";fixed.arithmetic_request->model_bundle.clear();fixed.arithmetic_request->model_bundle_sha256.clear();
            fixed.arithmetic_request->fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.arithmetic_request->fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.arithmetic_request->fixed_cost_table));
            auto control=pipelinev2::compile(fixed);CHECK(control.ok());std::ofstream(out/(stem+".C.json"))<<control.arithmetic_report;std::ofstream(out/(stem+".C.sv"))<<control.output_text;
            auto multiple=c;multiple.source_text->insert(multiple.source_text->find("void carryTop()"),"Int<9> z;\n#pragma output_port z\n");
            multiple.source_text->replace(multiple.source_text->find("y=AddCarry(a,b,carry);"),22,"y=AddCarry(a,b,carry);z=y;");
            auto rejected=pipelinev2::compile(multiple);CHECK(!rejected.ok());CHECK(rejected.arithmetic_diagnostic);CHECK(rejected.arithmetic_diagnostic->failure_code=="cost_uncovered");
            auto wiring=c;wiring.source_text->replace(wiring.source_text->find("y=AddCarry(a,b,carry);"),22,"y=Int<9>(a);");
            rejected=pipelinev2::compile(wiring);CHECK(!rejected.ok());CHECK(rejected.arithmetic_diagnostic);CHECK(rejected.arithmetic_diagnostic->failure_code=="cost_uncovered");
            auto asymmetric=c;asymmetric.source_text="#include <fixint.hpp>\nInt<8> a;Int<16> b;Int<24> y;bool carry,iv,ov;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port carry\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port ov\nvoid carryTop(){y=Int<24>(a)*Int<24>(b);}\n";
            rejected=pipelinev2::compile(asymmetric);CHECK(!rejected.ok());CHECK(rejected.arithmetic_diagnostic);CHECK(rejected.arithmetic_diagnostic->failure_code=="cost_uncovered");
        }
    }
    std::cout<<"Qualified model public-entry checks: "<<checks<<"\n";
}
