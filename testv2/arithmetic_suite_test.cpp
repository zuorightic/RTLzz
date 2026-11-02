#include "rtlzz.hpp"
#include "pipelinev2/PipelineV2.h"
#include "backend/arithmetic_core.hpp"
#include "backend/pipeline.hpp"
#include <llvm/Support/JSON.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <stdexcept>
static std::string read(const std::filesystem::path& p){std::ifstream f(p);if(!f)throw std::runtime_error("missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char** argv){
 if(argc<4)return 2;std::filesystem::path root=argv[1],manifest=argv[2],out=argv[3];std::filesystem::create_directories(out);auto parsed=llvm::json::parse(read(manifest));if(!parsed)return 2;
 unsigned passed=0;
 for(const auto& job:*parsed->getAsArray()){
  auto& j=*job.getAsObject();auto name=j.getString("name")->str();std::cerr<<"CASE "<<name<<"\n";
  pred::pipelinev2::PipelineConfig c;c.source_name=name+".logic.cpp";c.source_text=read(j.getString("source")->str());c.top_function=name;c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=argc>4;
  auto request=rtlzz::loadArithmeticRequest(j.getString("request")->str());c.arithmetic_request=request;
  c.pipeline.emplace();c.pipeline->register_stages=request.latency-1;c.pipeline->input_valid_name="iv";c.pipeline->output_valid_name="ov";c.pipeline->reset_enabled=true;
  for(const auto& [key,value]:*j.getObject("reset")){std::vector<std::uint64_t> words;for(const auto& w:*value.getAsArray())words.push_back(*w.getAsUINT64());c.pipeline->input_reset_values[key.str()]=words;}
  auto result=pred::pipelinev2::compile(c);if(!result.ok()){std::cerr<<result.error<<"\n"<<result.arithmetic_report<<"\n"<<result.error_debug_text;return 1;}
  pred::pipeline::validateProgram(*result.pipeline_program);
  for(const auto& region:result.pipeline_program->combinational.arithmetic_regions){
   if(region.operation=="div_unsigned"||region.operation=="mod_unsigned"||region.operation=="fp32_mul_rne"){
    if(region.internal_split_enabled)throw std::runtime_error("unregistered arithmetic internal split");
    const auto stage=result.pipeline_program->stages.at(region.outputs[0]);for(auto id:region.members)if(result.pipeline_program->stages.at(id)!=stage)throw std::runtime_error("atomic region cut");
   }
   if(region.operation=="div_unsigned"||region.operation=="mod_unsigned"){
    auto broken=result.pipeline_program->combinational;auto id=region.members.at(0);auto zero=broken.signal(id).driver->operands[1];zero.kind=pred::beir::OperandKind::Literal;zero.node=pred::beir::kInvalidNodeId;zero.constant.width=zero.type.width;zero.constant.limbs.assign((zero.type.width+63)/64,0);broken.signal(id).driver->operands[1]=zero;
    bool rejected=false;try{pred::beir::arithmetic::validate(broken,"unsafe_mutation");}catch(const rtlzz::ArithmeticError& error){rejected=error.diagnostic.failure_code=="unsafe_divisor";}
    if(!rejected)throw std::runtime_error("zero divisor graph was accepted");
   }
  }
  for(const auto& [key,value]:*j.getObject("expected_reset")){
   const auto& actual=result.pipeline_report->output_reset_values.at(key.str());std::vector<std::uint64_t> expected;for(const auto& w:*value.getAsArray())expected.push_back(*w.getAsUINT64());
   if(actual!=expected)throw std::runtime_error("wrong complete-limb reset value "+name+"/"+key.str());
  }
  std::ofstream(out/(name+".sv"))<<result.output_text;std::ofstream(out/(name+".json"))<<result.arithmetic_report;++passed;
 }
 std::cout<<"PASS arithmetic suite implementations="<<passed<<"\n";
}
