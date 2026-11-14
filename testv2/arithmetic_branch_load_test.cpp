#include "backend/arithmetic_branch_load.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace pred::beir::arithmetic;
int main(int argc,char** argv) {
    try {
        if(argc!=3)throw std::runtime_error("input/output required");
        std::ifstream in(argv[1]);std::string text{std::istreambuf_iterator<char>(in),{}};
        auto parsed=llvm::json::parse(text);
        if(!parsed||!parsed->getAsObject())throw std::runtime_error("request object required");
        auto& request=*parsed->getAsObject();auto queries=request.getArray("queries");auto kind=request.getString("kind");
        if(!queries||!kind)throw std::runtime_error("queries/kind required");
        llvm::json::Array result;
        for(const auto& raw:*queries) {
            auto q=raw.getAsObject();if(!q)throw std::runtime_error("query object required");
            if(auto model=request.getObject("model"))result.push_back(evaluateBranchLoad(*model,kind->str(),*q,request.getBoolean("diagnostic").value_or(false)));
            else if(*kind=="operator") {
                llvm::json::Object features;for(const auto& [n,v]:branchLoadOperatorFeatures(*q))features[n]=v;
                result.push_back(std::move(features));
            } else if(*kind=="G")result.push_back(branchLoadReductionFeatures(*q));
            else if(*kind=="S")result.push_back(branchLoadStateFeatures(*q));
            else throw std::runtime_error("unknown feature kind");
        }
        std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(result))).str()<<'\n';
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
