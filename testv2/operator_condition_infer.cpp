#include "backend/arithmetic_condition.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("input/output required");auto bytes=llvm::MemoryBuffer::getFile(argv[1]);if(!bytes)throw std::runtime_error("missing inference vectors");auto value=llvm::json::parse((*bytes)->getBuffer());if(!value||!value->getAsObject())throw std::runtime_error("invalid inference JSON");auto root=value->getAsObject();auto model=root->getObject("model");auto rows=root->getArray("queries");if(!model||!rows)throw std::runtime_error("missing model/queries");llvm::json::Array answers;
    for(const auto& row:*rows){auto request=row.getAsObject();if(!request)throw std::runtime_error("invalid request");auto d=request->getObject("descriptor");if(!d)throw std::runtime_error("descriptor required");auto r=pred::beir::arithmetic::operatorConditionEstimate(*model,*d,request->getBoolean("allow_candidate").value_or(false));llvm::json::Object answer{{"status",r.covered?"covered":"cost_uncovered"}};
        if(r.covered){answer["operator_delay_ps"]=r.operator_delay_ps;answer["delay_reserve_ps"]=r.delay_reserve_ps;answer["conservative_operator_delay_ps"]=r.operator_delay_ps+r.delay_reserve_ps;answer["comb_area_um2"]=r.comb_area_um2;answer["full_stage_price_qualified"]=false;}answers.push_back(std::move(answer));}
    std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(answers))).str()<<'\n';std::cout<<"CONDITION_INFERENCE_EXPORTED\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
