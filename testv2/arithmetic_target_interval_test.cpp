#include "backend/arithmetic_model.hpp"
#include "arithmetic_report_reader.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
using namespace pred;namespace fs=std::filesystem;
static std::string read(const fs::path& path){std::ifstream f(path);return {std::istreambuf_iterator<char>(f),{}};}
int main(int argc,char**argv){
 if(argc!=5){std::cerr<<"root model graphs output\n";return 1;}
 fs::path root=argv[1],model=argv[2],graphs=argv[3],out=argv[4];llvm::json::Array rows;
 std::vector<fs::path> files;for(const auto& item:fs::directory_iterator(graphs))if(item.path().extension()==".json")files.push_back(item.path());std::sort(files.begin(),files.end());
 for(const auto& file:files){
  auto parsed=llvm::json::parse(read(file));if(!parsed||!parsed->getAsObject())return 2;
  auto p=arithmetic_replay::graph(*parsed->getAsObject());const auto& region=p.arithmetic_regions.at(0);
  int width=region.template_parameters.at("lhs_width");if(width<8||width>32)continue;
  for(double period:{1500.,1875.,2250.,1.,2251.}){
   rtlzz::ArithmeticRequest r;r.target_platform="asap7";r.cost_mode="backend-model";r.target_period_ps=period;r.latency=3;
   r.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();r.platform_profile_sha256=rtlzz::arithmeticHash(read(r.platform_profile));
   r.model_bundle=model.string();r.model_bundle_sha256=rtlzz::arithmeticHash(read(model));r.candidate_policy="mul-candidates-v1";
   for(std::size_t first=0;first<region.legal_cut_groups.size();++first)for(std::size_t last=first;last<region.legal_cut_groups.size();++last){
    llvm::json::Object row{{"graph",file.filename().string()},{"width",width},{"first",static_cast<std::int64_t>(first)},{"last",static_cast<std::int64_t>(last)},{"period_ps",period}};
    try{beir::arithmetic::ModelCostProvider provider(p,r);std::vector<beir::NodeId> members;
     for(std::size_t g=first;g<=last;++g)members.insert(members.end(),region.legal_cut_groups[g].begin(),region.legal_cut_groups[g].end());
     bool covered=provider.stageShapeCovered(members);row["covered"]=covered;
     if(covered){row["delay_ps"]=provider.stageDelay(0,members.size(),0,members);row["comb_area_um2"]=provider.stageArea(0,members.size(),0,members);}
    }catch(const rtlzz::ArithmeticError& error){row["covered"]=false;row["failure_code"]=error.diagnostic.failure_code;row["message"]=std::string(error.what());}
    rows.push_back(std::move(row));
   }
  }
 }
 std::string text;llvm::raw_string_ostream stream(text);stream<<llvm::json::Value(llvm::json::Object{{"candidate_PPA_calls",0},{"rows",std::move(rows)}});stream.flush();std::ofstream(out)<<text<<"\n";
 std::cout<<"Native target-conditioned interval queries completed\n";return 0;
}
