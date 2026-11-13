#pragma once
#include <llvm/Support/JSON.h>
#include <map>
#include <string>
#include <vector>
namespace pred::beir::arithmetic {
struct WideAluGeometry {
    std::map<std::string,double> features;
    std::vector<int> lhs_bits,rhs_bits,dynamic_outputs;
    bool mul=false;
};
// The spec must first pass the public wide Query validator.
WideAluGeometry demandedWideAluGeometry(const llvm::json::Object& spec);
}
