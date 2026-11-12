#include "backend/arithmetic_reduction_model.hpp"
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
using namespace pred::beir::arithmetic;using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static std::string read(const char* path){auto b=llvm::MemoryBuffer::getFile(path);if(!b)throw std::runtime_error("missing input");return (*b)->getBuffer().str();}
int main(int argc,char** argv){try{if(argc!=5&&!(argc==6&&std::string(argv[5])=="--candidate-parity"))throw std::runtime_error("model queries profile_sha256 output [--candidate-parity] required");auto raw=read(argv[1]);ReductionCostModel model(argv[1],rtlzz::arithmeticHash(raw),argv[3],argc==6?ReductionUse::CandidateParity:ReductionUse::Production);auto q=llvm::json::parse(read(argv[2]));if(!q||!q->getAsArray())throw std::runtime_error("query array required");A rows;
    for(const auto& item:*q->getAsArray()){O row;try{const auto& x=*item.getAsObject();ReductionRegion r;r.width=int(*x.getInteger("output_bits"));for(const auto& t:*x.getArray("terms")){std::vector<ReductionBit> term;for(const auto& v:*t.getAsArray()){const auto& b=*v.getAsArray();term.push_back({pred::beir::NodeId(*b[0].getAsInteger()),int(*b[1].getAsInteger()),int(*b[2].getAsInteger())});}r.terms.push_back(std::move(term));}
        double target=*x.getNumber("target_ps");auto p=model.price(r,target);O f;for(auto [n,v]:reductionFeatures(r.width,r.terms,target))f[n]=v;row["status"]="covered";row["delay_ps"]=p.delay_ps;row["area_um2"]=p.area_um2;row["features"]=std::move(f);
    }catch(const std::exception& e){row["status"]="cost_uncovered";row["reason"]=std::string(e.what());}rows.push_back(std::move(row));}
    std::ofstream(argv[4])<<llvm::formatv("{0:2}",V(std::move(rows))).str()<<'\n';std::cout<<"G_CONSUMER_COMPLETE queries="<<q->getAsArray()->size()<<'\n';
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
