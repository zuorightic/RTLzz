#pragma once
#include <llvm/Support/JSON.h>
namespace pred::beir::arithmetic {
llvm::json::Array prospectiveControlFeatures(const llvm::json::Object& structure);
llvm::json::Object evaluateControlSuccessor(const llvm::json::Object& model,
                                          const llvm::json::Object& structure,
                                          bool diagnostic = false);
}
