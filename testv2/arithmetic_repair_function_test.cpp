#include "backend/arithmetic_word_reduction.hpp"
#include "backend/arithmetic_operator_successor.hpp"
#include "backend/arithmetic_control_response_v4.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pred::beir::arithmetic;
int main(int argc,char** argv) {try {
    if(argc!=3)throw std::runtime_error("input/output required");std::ifstream in(argv[1]);std::string text{std::istreambuf_iterator<char>(in),{}};auto parsed=llvm::json::parse(text);
    if(!parsed||!parsed->getAsObject())throw std::runtime_error("request required");auto& request=*parsed->getAsObject();auto queries=request.getArray("queries");auto kind=request.getString("kind");
    if(!queries||!kind)throw std::runtime_error("queries/kind required");llvm::json::Array result;
    for(const auto& raw:*queries) {auto q=raw.getAsObject();if(!q)throw std::runtime_error("query required");auto m=request.getObject("model");
        if(*kind=="operator") {if(!m)throw std::runtime_error("operator model required");result.push_back(evaluateOperatorSuccessor(*m,*q,request.getBoolean("diagnostic").value_or(false)));}
        else if(*kind=="G") {
            if(m)result.push_back(evaluateWordReduction(*m,*q,request.getBoolean("diagnostic").value_or(false)));
            else {auto T=q->getNumber("target_ps");if(!T)throw std::runtime_error("target required");auto f=wordReductionFeatures(*q,*T);llvm::json::Object a,d;for(const auto& [n,x]:f.area)a[n]=x;for(const auto& [n,x]:f.delay)d[n]=x;result.push_back(llvm::json::Object{{"area",std::move(a)},{"delay",std::move(d)}});}
        }else if(*kind=="G6") {
            if(m)result.push_back(evaluateMappingReduction(*m,*q,request.getBoolean("diagnostic").value_or(false)));
            else {auto T=q->getNumber("target_ps");if(!T)throw std::runtime_error("target required");auto f=mappingReductionFeatures(*q,*T);llvm::json::Object a,d,z;for(const auto& [n,x]:f.area)a[n]=x;for(const auto& [n,x]:f.delay)d[n]=x;for(const auto& [n,x]:f.intensive)z[n]=x;result.push_back(llvm::json::Object{{"area",std::move(a)},{"delay",std::move(d)},{"intensive",std::move(z)}});}
        }else if(*kind=="S") {
            if(!m)throw std::runtime_error("S model required");
            result.push_back(evaluateControlResponseV4(*m,*q,request.getBoolean("diagnostic").value_or(false)));
        }else throw std::runtime_error("unknown repair consumer kind");
    }
    std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(result))).str()<<'\n';return 0;
}catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}}
