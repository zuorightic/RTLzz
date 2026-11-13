#include "backend/arithmetic_wide_function.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pred::beir::arithmetic;
int main(int argc,char** argv){try{
 if(argc!=3)throw std::runtime_error("input/output required");std::ifstream in(argv[1]);std::string text{std::istreambuf_iterator<char>(in),{}};auto parsed=llvm::json::parse(text);if(!parsed||!parsed->getAsObject())throw std::runtime_error("request required");const auto& req=*parsed->getAsObject();auto qs=req.getArray("queries");if(!qs)throw std::runtime_error("queries required");llvm::json::Array out;
 for(const auto& v:*qs){auto q=v.getAsObject();if(!q)throw std::runtime_error("query required");if(auto m=req.getObject("model"))out.push_back(evaluateWideFunctionModel(*m,*q,req.getBoolean("diagnostic").value_or(false)));else{llvm::json::Object f;for(const auto& [n,x]:wideFunctionQueryFeatures(*q))f[n]=x;out.push_back(std::move(f));}}
 std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(out))).str()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
