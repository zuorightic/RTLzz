#include "backend/arithmetic_reset_function.hpp"
#include <llvm/Support/raw_ostream.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace pred::beir::arithmetic;
static llvm::json::Value load(const char* p){std::ifstream f(p);if(!f)throw std::runtime_error("missing input");std::string text{std::istreambuf_iterator<char>(f),{}};auto v=llvm::json::parse(text);if(!v)throw std::runtime_error(llvm::toString(v.takeError()));return std::move(*v);}
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("model and numerical query JSON required");
    auto m=load(argv[1]),q=load(argv[2]);if(!m.getAsObject()||!q.getAsArray())throw std::runtime_error("invalid input schema");llvm::json::Array results;
    for(const auto& v:*q.getAsArray()){
        auto row=v.getAsObject();if(!row)throw std::runtime_error("invalid numerical request");
        auto ff=row->getNumber("total_FF_bits"),ones=row->getNumber("reset_one_bits"),t=row->getNumber("target_ps");auto depth=row->getInteger("native_depth");
        if(!ff||!ones||!t||!depth)throw std::runtime_error("missing reset features");
        auto r=resetTimingPrice(*m.getAsObject(),*ff,*ones,*t,int(*depth));
        llvm::json::Object out{{"covered",r.covered},{"reason",r.reason}};
        if(r.covered){out["point_intrinsic_ps"]=r.point_intrinsic_ps;out["guard_intrinsic_ps"]=r.guard_intrinsic_ps;out["required_period_ps"]=r.required_period_ps;out["violates"]=r.violates;}
        results.push_back(std::move(out));
    }
    llvm::outs()<<llvm::json::Value(std::move(results))<<"\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<"\n";return 1;}}
