#include "rtlzz.hpp"
#include "pipelinev2/PipelineV2.h"
#include "backend/arithmetic_core.hpp"
#include "backend/arithmetic_selection.hpp"
#include "backend/mul_split.hpp"
#include "backend/arithmetic_regions.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <cmath>
using namespace pred;using namespace beir;
static int checks=0;
#define CHECK(x) do {++checks;if(!(x)){std::cerr<<__LINE__<<": "<<#x<<"\n";return 1;}}while(false)
static std::string read(const std::filesystem::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
static rtlzz::ArithmeticRequest request(const std::filesystem::path& root,int L){
 rtlzz::ArithmeticRequest r;r.target_platform="asap7";r.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();r.platform_profile_sha256=rtlzz::arithmeticHash(read(r.platform_profile));r.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();r.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(r.fixed_cost_table));r.latency=L;r.target_period_ps=1500;return r;
}
int main(int argc,char** argv){
 CHECK(argc==3||argc==4);bool circt=argc==4;std::filesystem::path root=argv[1],out=argv[2];std::filesystem::create_directories(out);llvm::json::Array summary;
 for(int w:{8,16,32,64,128,12,24,48,96,13,17})for(int product:{0,1})for(int mode:{0,1,2,3}){
  if(circt && (w!=8 && w!=128 || mode==0))continue;
  const int R=product?w:2*w;std::string stem="w"+std::to_string(w)+"_"+(product?"low":"full")+"_"+std::to_string(mode);
  pipelinev2::PipelineConfig c;c.source_name="mul-template.logic.cpp";c.source_text="#include <fixint.hpp>\nInt<"+std::to_string(w)+"> a,b,c; bool iv,ov; Int<"+std::to_string(R)+"> y,z;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port c\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port z\n#pragma output_port ov\nvoid top(){y=a*b;z=a*c;}\n";c.use_circt=circt;c.top_function=stem;c.source_text->replace(c.source_text->find("void top()"),10,"void "+stem+"()");c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.arithmetic_request=request(root,3);c.pipeline.emplace();c.pipeline->register_stages=2;c.pipeline->input_valid_name="iv";c.pipeline->output_valid_name="ov";c.pipeline->reset_enabled=true;c.pipeline->input_reset_values={{"a",{5}},{"b",{7}},{"c",{11}},{"iv",{0}}};
  for(auto& [name,limbs]:c.pipeline->input_reset_values)if(name!="iv")limbs.resize((w+63)/64,0);
  if(mode==1||mode==2){c.arithmetic_request->candidate_policy="mul-template-v1";c.arithmetic_request->template_parameters={{"block_width",std::max(2,w/(mode==1?2:4))}};}
  if(mode==3)c.arithmetic_request->candidate_policy="mul-candidates-v1";
  auto r=pipelinev2::compile(c);if(!r.ok())std::cerr<<stem<<": "<<r.error<<"\n"<<r.arithmetic_report;CHECK(r.ok());CHECK(r.pipeline_program);pipeline::validateProgram(*r.pipeline_program);
  CHECK(r.pipeline_program->combinational.arithmetic_regions.size()==2);
  bool internal=false;for(const auto& region:r.pipeline_program->combinational.arithmetic_regions){
   for(const auto& group:regions::schedulingGroups(r.pipeline_program->combinational))for(auto id:group)CHECK(r.pipeline_program->stages[id]==r.pipeline_program->stages[group[0]]);
   if(region.internal_split_enabled){CHECK(region.legal_cut_groups.size()>=3);std::set<int> levels;for(auto id:region.members)levels.insert(r.pipeline_program->stages[id]);internal|=levels.size()>1;CHECK(!region.internal_registers.size());}
  }
  if(mode==1||mode==2)CHECK(internal);
  auto json=llvm::json::parse(r.arithmetic_report);CHECK(static_cast<bool>(json));auto* selection=json->getAsObject()->getObject("selection");CHECK(selection);CHECK(selection->getBoolean("formal_C_selection")==static_cast<bool>(mode==3));
  if(mode==3)CHECK(selection->getInteger("evaluated_structures")==9);
  auto bad=*r.pipeline_program;bad.rtl_program.arithmetic_regions.clear();bool rejected=false;try{pipeline::validateProgram(bad);}catch(const std::exception&){rejected=true;}CHECK(rejected);
  if(mode==1||mode==2){auto invalid=*r.pipeline_program;const auto& group=invalid.combinational.arithmetic_regions[0].legal_cut_groups[0];invalid.stages[group[0]]=(invalid.stages[group[0]]+1)%3;rejected=false;try{pipeline::validateProgram(invalid);}catch(const std::exception&){rejected=true;}CHECK(rejected);}
  if(w==8 && product==0 && mode==1) {
   const auto& graph=r.pipeline_program->combinational;auto stages=r.pipeline_program->stages;
   std::set<NodeId> members;for(const auto& region:graph.arithmetic_regions)members.insert(region.members.begin(),region.members.end());
   llvm::json::Array placements;int admissible=0;
   const auto& groups=graph.arithmetic_regions[0].legal_cut_groups;
   for(int a=0;a<=2;++a)for(int b=a;b<=2;++b)for(int c=b;c<=2;++c){
    auto proposed=stages;int values[]{a,b,c};for(int k=0;k<3;++k)for(auto id:groups[k])proposed[id]=values[k];
    for(std::size_t pass=0;pass<graph.signals.size();++pass)for(const auto& node:graph.signals)if(!members.count(node.id))for(const auto& operand:node.driver->operands)if(operand.kind==OperandKind::Symbol)proposed[node.id]=std::max(proposed[node.id],proposed[operand.node]);
    const auto evaluated=pipeline::evaluateSchedule(graph,r.pipeline_program->options,proposed);
    if(!std::isfinite(evaluated.final_delay))continue;
    llvm::json::Array map,delays;for(int s:proposed)map.push_back(s);for(double d:evaluated.stage_delays)delays.push_back(d);
    bool accepted=evaluated.final_delay<=r.pipeline_report->allowed_delay;admissible+=accepted;
    placements.push_back(llvm::json::Object{{"stages",std::move(map)},{"stage_delays",std::move(delays)},{"maximum_delay",evaluated.final_delay},{"register_bits",evaluated.register_bits},{"register_count",static_cast<std::uint64_t>(evaluated.register_count)},{"within_production_cap",accepted}});
   }
   auto insufficient=r.pipeline_program->options;insufficient.register_stages=0;insufficient.arithmetic_request->latency=1;
   try{pipeline::requireSchedule(graph,insufficient,2);CHECK(false);}catch(const rtlzz::ArithmeticError& e){CHECK(e.diagnostic.failure_code=="schedule_no_placement_found");}
   CHECK(admissible>=2);std::ofstream(out/"positions.json")<<llvm::formatv("{0:2}",llvm::json::Value(std::move(placements))).str();
  }
  auto options=r.pipeline_program->options;
  try{pipeline::requireSchedule(r.pipeline_program->combinational,options,0);CHECK(false);}catch(const rtlzz::ArithmeticError& e){CHECK(e.diagnostic.failure_code=="schedule_group_over_budget");}
  std::ofstream(out/(stem+".sv"))<<r.output_text;std::ofstream(out/(stem+".json"))<<r.arithmetic_report;
  summary.push_back(llvm::json::Object{{"stem",stem},{"W",w},{"R",R},{"mode",mode},{"internal_registers",internal},{"register_bits",r.pipeline_report->register_bits}});
 }
 if(!circt)for(int w:{8,16,32,64,128})for(int family:{0,1})for(int mode:{0,1,2,3}) {
  std::string name="sensitivity_"+std::to_string(w)+"_"+std::to_string(family)+"_"+std::to_string(mode);
  pipelinev2::PipelineConfig c;c.source_name="sensitivity.logic.cpp";c.top_function="sensitivity_"+std::to_string(w)+"_"+std::to_string(family);c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};
  c.source_text="#include <fixint.hpp>\nInt<"+std::to_string(w)+"> a,b;Int<"+std::to_string(2*w+1)+"> c,y;bool iv,ov;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port c\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port ov\nvoid "+c.top_function+"(){Int<"+std::to_string(2*w)+"> p=a*b;y="+(family?"Int<"+std::to_string(2*w+1)+">(p)+c":"p")+";}";
  c.arithmetic_request=request(root,3);c.pipeline.emplace();c.pipeline->register_stages=2;c.pipeline->input_valid_name="iv";c.pipeline->output_valid_name="ov";
  if(mode==1||mode==2){c.arithmetic_request->candidate_policy="mul-template-v1";c.arithmetic_request->template_parameters={{"block_width",std::max(2,w/(mode==1?2:4))}};}
  if(mode==3)c.arithmetic_request->candidate_policy="mul-candidates-v1";
  auto r=pipelinev2::compile(c);if(!r.ok())std::cerr<<name<<": "<<r.error;CHECK(r.ok());
  std::ofstream(out/(name+".json"))<<r.arithmetic_report;std::ofstream(out/(name+".sv"))<<r.output_text;
 }
 if(!circt) {
  pipelinev2::PipelineConfig c;c.source_name="K16.logic.cpp";c.source_text="#include <fixint.hpp>\nInt<16> a,b,c;Int<32> x,y,z;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port c\n#pragma output_port x\n#pragma output_port y\n#pragma output_port z\nvoid K16(){x=a*b;y=a*c;z=b*c;}";
  c.top_function="K16";c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.arithmetic_request=request(root,3);c.arithmetic_request->candidate_policy="mul-candidates-v1";c.pipeline.emplace();c.pipeline->register_stages=2;
  auto r=pipelinev2::compile(c);CHECK(r.ok());auto parsed=llvm::json::parse(r.arithmetic_report);CHECK(static_cast<bool>(parsed));CHECK(parsed->getAsObject()->getObject("selection")->getInteger("evaluated_structures")==16);
  CHECK(r.beir_program->arithmetic_regions.size()==r.pipeline_program->combinational.arithmetic_regions.size());
  for(std::size_t i=0;i<r.beir_program->arithmetic_regions.size();++i)CHECK(r.beir_program->arithmetic_regions[i].template_version==r.pipeline_program->combinational.arithmetic_regions[i].template_version);
  std::ofstream(out/"K16.json")<<r.arithmetic_report;std::ofstream(out/"K16.sv")<<r.output_text;
 }
 std::ofstream(out/"summary.json")<<llvm::formatv("{0:2}",llvm::json::Value(std::move(summary))).str();
 std::cout<<"PASS parameterized Mul checks="<<checks<<"\n";
}
