#include "backend/arithmetic_reduction_merge.hpp"
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <iterator>
using namespace pred::beir::arithmetic;
int main(int argc,char** argv){try{
    if(argc!=3)throw std::runtime_error("queries and new output required");
    std::ifstream input(argv[1]);std::string text{std::istreambuf_iterator<char>(input),{}};auto parsed=llvm::json::parse(text);
    if(!parsed||!parsed->getAsArray())throw std::runtime_error("query array required");llvm::json::Array rows;
    for(const auto& value:*parsed->getAsArray()){
        const auto& q=*value.getAsObject();std::vector<std::vector<ReductionBit>> terms;
        for(const auto& raw:*q.getArray("terms")){std::vector<ReductionBit> term;
            for(const auto& bit:*raw.getAsArray()){const auto& a=*bit.getAsArray();term.push_back({std::size_t(*a[0].getAsInteger()),int(*a[1].getAsInteger()),int(*a[2].getAsInteger())});}terms.push_back(std::move(term));}
        llvm::json::Object f;for(const auto& [k,v]:reductionMergeFeatures(int(*q.getInteger("output_bits")),terms,*q.getNumber("target_ps")))f[k]=v;
        rows.push_back(std::move(f));
    }
    std::ofstream(argv[2])<<llvm::formatv("{0:2}",llvm::json::Value(std::move(rows))).str()<<'\n';
    std::cout<<"REDUCTION_MERGE_FEATURES_EXPORTED\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
