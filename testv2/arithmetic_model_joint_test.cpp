#include "rtlzz.hpp"
#include "arithmetic_request.hpp"
#include "pipelinev2/PipelineV2.h"
#include "backend/pipeline.hpp"
#include "backend/arithmetic_model.hpp"
#include "arithmetic_report_reader.hpp"
#include <llvm/Support/JSON.h>
#include <filesystem>
#include <fstream>
#include <iostream>
using namespace pred;namespace fs=std::filesystem;
static unsigned checks=0;
#define CHECK(x) do{++checks;if(!(x)){std::cerr<<__LINE__<<": "<<#x<<"\n";return 1;}}while(false)
static std::string read(const fs::path&p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char**argv){CHECK(argc==4);fs::path root=argv[1],models=argv[2],out=argv[3];fs::create_directories(out);
 for(const std::string platform:{"asap7","nangate45","sky130hd","gf180"}){
  rtlzz::ArithmeticRequest r;r.target_platform=platform;r.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms"/(platform+".json")).string();r.platform_profile_sha256=rtlzz::arithmeticHash(read(r.platform_profile));r.cost_mode="backend-model";r.model_bundle=(models/(platform+".json")).string();r.model_bundle_sha256=rtlzz::arithmeticHash(read(r.model_bundle));r.candidate_policy="mul-candidates-v1";r.latency=3;
  auto model=llvm::json::parse(read(r.model_bundle));CHECK(static_cast<bool>(model));r.target_period_ps=*model->getAsObject()->getNumber("mapping_abc_target_ps");
  pipelinev2::PipelineConfig c;c.source_name="model-joint-source.logic.cpp";c.source_text="#include <fixint.hpp>\nInt<8> a,b;Int<16> y;bool iv,ov;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port ov\nvoid top(){y=a*b;}\n";c.top_function="top";c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.arithmetic_request=r;c.pipeline.emplace();c.pipeline->register_stages=2;c.pipeline->reset_enabled=true;c.pipeline->input_valid_name="iv";c.pipeline->output_valid_name="ov";c.pipeline->input_reset_values={{"a",{0}},{"b",{0}},{"iv",{0}}};
  auto result=pipelinev2::compile(c);if(!result.ok())std::cerr<<result.error<<result.arithmetic_report;CHECK(result.ok());CHECK(result.pipeline_program);pipeline::validateProgram(*result.pipeline_program);
  auto report=llvm::json::parse(result.arithmetic_report);CHECK(static_cast<bool>(report));auto selection=report->getAsObject()->getObject("selection");CHECK(selection);CHECK(selection->getBoolean("formal_D_selection")==true);CHECK(selection->getBoolean("candidate_model_coverage_complete")==true);CHECK(selection->getInteger("evaluated_structures")==3);CHECK(selection->getInteger("candidate_PPA_calls")==0);
  std::ofstream(out/(platform+".report.json"))<<result.arithmetic_report;std::ofstream(out/(platform+".sv"))<<result.output_text;
  auto candidates=selection->getArray("candidates");CHECK(candidates);
  for(const auto& entry:*candidates){auto row=entry.getAsObject();CHECK(row);if(row->getString("id")!="candidate/half")continue;
    auto computation=row->getObject("computation");CHECK(computation);auto graph=arithmetic_replay::graph(*computation);const auto& region=graph.arithmetic_regions.at(0);
    beir::arithmetic::ModelCostProvider provider(graph,r);CHECK(provider.stageShapeCovered(region.legal_cut_groups.front()));
    auto partial=region.legal_cut_groups.front();partial.pop_back();CHECK(!provider.stageShapeCovered(partial));
    auto changed=graph;std::swap(changed.arithmetic_regions[0].legal_cut_groups[0],changed.arithmetic_regions[0].legal_cut_groups[1]);bool refused=false;
    try{beir::arithmetic::ModelCostProvider bad(changed,r);}catch(const rtlzz::ArithmeticError&){refused=true;}CHECK(refused);
    changed=graph;bool mutated=false;for(auto id:changed.arithmetic_regions[0].members){auto& d=*changed.signal(id).driver;if(d.kind==beir::OperationKind::Slice&&d.lo>0){d.hi-=d.lo;d.lo=0;mutated=true;break;}}CHECK(mutated);refused=false;
    try{beir::arithmetic::ModelCostProvider bad(changed,r);}catch(const rtlzz::ArithmeticError&){refused=true;}CHECK(refused);
  }
  auto fixed=c;fixed.arithmetic_request->cost_mode="fixed";fixed.arithmetic_request->model_bundle.clear();fixed.arithmetic_request->model_bundle_sha256.clear();fixed.arithmetic_request->fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.arithmetic_request->fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.arithmetic_request->fixed_cost_table));auto control=pipelinev2::compile(fixed);CHECK(control.ok());std::ofstream(out/(platform+".C.report.json"))<<control.arithmetic_report;std::ofstream(out/(platform+".C.sv"))<<control.output_text;
 }
 std::cout<<"Joint model public-entry checks: "<<checks<<"\n";
}
