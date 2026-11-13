#include "backend/arithmetic_operator_successor.hpp"
#include "backend/arithmetic_wide_reduction.hpp"
#include "backend/arithmetic_control_successor.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pred::beir::arithmetic;
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("input/output required");std::ifstream in(argv[1]);std::string data{std::istreambuf_iterator<char>(in),{}};
    auto parsed=llvm::json::parse(data);if(!parsed||!parsed->getAsObject())throw std::runtime_error("request required");auto& request=*parsed->getAsObject();
    auto queries=request.getArray("queries");auto kind=request.getString("kind");if(!queries||!kind)throw std::runtime_error("queries/kind required");llvm::json::Array out;
    for(const auto& raw:*queries){auto q=raw.getAsObject();if(!q)throw std::runtime_error("query required");auto m=request.getObject("model");
        if(*kind=="operator"){
            if(m)out.push_back(evaluateOperatorSuccessor(*m,*q,request.getBoolean("diagnostic").value_or(false)));
            else{llvm::json::Object f;for(const auto& [n,x]:successorOperatorFeatures(*q))f[n]=x;out.push_back(std::move(f));}
        }else if(*kind=="G"){
            if(m)out.push_back(evaluateWideReduction(*m,*q,request.getBoolean("diagnostic").value_or(false),request.getBoolean("extrapolate").value_or(false)));
            else{auto t=q->getNumber("target_ps");if(!t)throw std::runtime_error("G target required");auto f=wideReductionFeatures(*q,*t);llvm::json::Object a,d;for(const auto& [n,x]:f.area)a[n]=x;for(const auto& [n,x]:f.delay)d[n]=x;out.push_back(llvm::json::Object{{"area",std::move(a)},{"delay",std::move(d)}});}
        }else if(*kind=="S"){
            if(m)out.push_back(evaluateControlSuccessor(*m,*q,request.getBoolean("diagnostic").value_or(false)));
            else out.push_back(prospectiveControlFeatures(*q));
        }else throw std::runtime_error("unknown numerical successor kind");
    }
    std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(out))).str()<<'\n';return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
