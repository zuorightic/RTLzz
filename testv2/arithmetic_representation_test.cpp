#include "backend/arithmetic_add_normalization.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pred::beir::arithmetic;
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("input/output required");std::ifstream in(argv[1]);std::string text{std::istreambuf_iterator<char>(in),{}};auto parsed=llvm::json::parse(text);if(!parsed||!parsed->getAsArray())throw std::runtime_error("query array required");llvm::json::Array results;
    for(const auto& value:*parsed->getAsArray()){auto q=value.getAsObject();if(!q)throw std::runtime_error("query object required");auto normalized=normalizeAddQuery(*q);auto features=normalizedAddFeatures(*q);llvm::json::Object f;for(const auto& [n,v]:features)f[n]=v;results.push_back(llvm::json::Object{{"normalization",std::move(normalized)},{"features",std::move(f)},{"rtl",normalizedAddCore(*q,"NormalizedCore")}});}
    std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(results))).str()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
