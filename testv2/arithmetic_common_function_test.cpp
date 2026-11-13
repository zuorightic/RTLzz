#include "backend/arithmetic_common_features.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pred::beir::arithmetic;
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("input and new output required");std::ifstream stream(argv[1]);std::string text{std::istreambuf_iterator<char>(stream),{}};auto parsed=llvm::json::parse(text);if(!parsed||!parsed->getAsArray())throw std::runtime_error("query array required");llvm::json::Array out;
    for(const auto& raw:*parsed->getAsArray()){auto q=raw.getAsObject();if(!q)throw std::runtime_error("query object required");std::map<std::string,double> f;
        if(auto d=q->getObject("descriptor"))f=commonOperatorFeatures(*d);
        else if(auto widths=q->getArray("input_widths")){std::vector<int> w;std::vector<std::vector<int>> sinks;for(const auto& v:*widths)w.push_back(int(*v.getAsInteger()));for(const auto& row:*q->getArray("sink_inputs")){std::vector<int> bits;for(const auto& v:*row.getAsArray())bits.push_back(int(*v.getAsInteger()));sinks.push_back(std::move(bits));}f=logicalInputLoadFeatures(w,sinks);}
        else {auto values=q->getObject("features");if(!values)throw std::runtime_error("numerical features required");for(const auto& value:*values){auto n=value.second.getAsNumber();if(!n)throw std::runtime_error("numerical feature required");f[value.first.str()]=*n;}}
        llvm::json::Object answer,features;for(const auto& [k,v]:f)features[k]=v;answer["features"]=std::move(features);if(auto model=q->getObject("model"))answer["prediction"]=commonNumericalPrediction(*model,f);out.push_back(std::move(answer));}
    std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(out))).str()<<'\n';std::cout<<"COMMON_NUMERICAL_FUNCTION_EXPORTED\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
