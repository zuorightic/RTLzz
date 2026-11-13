#include "backend/arithmetic_prefix_features.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pred::beir::arithmetic;
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("input/output required");std::ifstream in(argv[1]);std::string text{std::istreambuf_iterator<char>(in),{}};auto parsed=llvm::json::parse(text);if(!parsed||!parsed->getAsArray())throw std::runtime_error("array required");llvm::json::Array out;
    for(const auto& raw:*parsed->getAsArray()){auto q=raw.getAsObject();if(!q||!q->getNumber("target_ps"))throw std::runtime_error("target required");llvm::json::Object f;for(const auto& [n,v]:livePrefixReductionFeatures(*q,*q->getNumber("target_ps")))f[n]=v;out.push_back(std::move(f));}
    std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(out))).str()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
