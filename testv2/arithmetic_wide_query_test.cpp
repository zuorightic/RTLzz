#include "backend/arithmetic_wide_query.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pred::beir::arithmetic;
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("input/output required");
    std::ifstream in(argv[1]);std::string text{std::istreambuf_iterator<char>(in),{}};
    auto parsed=llvm::json::parse(text);if(!parsed||!parsed->getAsArray())throw std::runtime_error("query array required");
    llvm::json::Array results;
    for(const auto& value:*parsed->getAsArray()){
        auto input=value.getAsObject();if(!input)throw std::runtime_error("case required");
        llvm::json::Object output;
        try{
            auto q=input->getObject("query");if(!q)throw std::runtime_error("query required");
            llvm::json::Object f;for(const auto& [n,v]:wideOperatorQueryFeatures(*q))f[n]=v;
            output["features"]=std::move(f);output["rtl"]=wideOperatorCore(*q,"WideCase");
            llvm::json::Array values;
            if(auto vectors=input->getArray("vectors"))for(const auto& raw:*vectors){
                auto v=raw.getAsObject();if(!v||!v->getString("a")||!v->getString("b")||!v->getBoolean("control"))throw std::runtime_error("vector required");
                values.push_back(evaluateWideOperator(*q->getObject("local_context")->getObject("producer"),v->getString("a")->str(),v->getString("b")->str(),*v->getBoolean("control")));
            }
            output["values"]=std::move(values);output["accepted"]=true;
        }catch(const std::exception& e){output=llvm::json::Object{{"accepted",false},{"reason",std::string(e.what())}};}
        results.push_back(std::move(output));
    }
    std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(results))).str()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
