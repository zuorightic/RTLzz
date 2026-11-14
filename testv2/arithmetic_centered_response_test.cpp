#include "backend/arithmetic_centered_response.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pred::beir::arithmetic;
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("input/output required");std::ifstream in(argv[1]);std::string text{std::istreambuf_iterator<char>(in),{}};auto parsed=llvm::json::parse(text);
    if(!parsed||!parsed->getAsObject())throw std::runtime_error("request object required");auto& req=*parsed->getAsObject();auto queries=req.getArray("queries");auto kind=req.getString("kind");if(!queries||!kind)throw std::runtime_error("queries/kind required");llvm::json::Array result;
    for(const auto& raw:*queries){auto q=raw.getAsObject();if(!q)throw std::runtime_error("query object required");auto m=req.getObject("model");
        if(m)result.push_back(evaluateCommonCentered(*m,kind->str(),*q,req.getBoolean("diagnostic").value_or(false)));
        else if(*kind=="operator"){llvm::json::Object f;for(const auto& [n,v]:commonMechanismOperatorFeatures(*q))f[n]=v;result.push_back(std::move(f));}
        else if(*kind=="G")result.push_back(commonMechanismReductionFeatures(*q));
        else if(*kind=="S")result.push_back(commonMechanismStateFeatures(*q));
        else throw std::runtime_error("unknown feature kind");
    }
    std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(result))).str()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
