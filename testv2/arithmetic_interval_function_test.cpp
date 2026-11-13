#include "backend/arithmetic_interval_features.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pred::beir::arithmetic;
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("input and output required");std::ifstream stream(argv[1]);std::string text{std::istreambuf_iterator<char>(stream),{}};auto parsed=llvm::json::parse(text);if(!parsed||!parsed->getAsArray())throw std::runtime_error("query array required");llvm::json::Array out;
    auto vector=[](const llvm::json::Object& o,const char* key){auto a=o.getArray(key);if(!a)throw std::runtime_error("missing control vector");std::vector<int> v;for(const auto& x:*a){auto n=x.getAsInteger();if(!n)throw std::runtime_error("integer control load required");v.push_back(int(*n));}return v;};
    for(const auto& raw:*parsed->getAsArray()){auto q=raw.getAsObject();if(!q)throw std::runtime_error("query object required");std::map<std::string,double> f;
        if(auto d=q->getObject("descriptor"))f=intervalOperatorFeatures(*d);
        else if(auto c=q->getObject("control"))f=stateControlLoadFeatures(vector(*c,"input_widths"),vector(*c,"stage_enabled_data_bits"),int(*c->getInteger("external_data_bits")),int(*c->getInteger("service_mode")));
        else throw std::runtime_error("unknown interval query");llvm::json::Object a,features;for(const auto& [k,v]:f)features[k]=v;a["features"]=std::move(features);if(auto m=q->getObject("model"))a["prediction"]=commonNumericalPrediction(*m,f);out.push_back(std::move(a));}
    std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(out))).str()<<'\n';std::cout<<"INTERVAL_NUMERICAL_FUNCTION_EXPORTED\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
