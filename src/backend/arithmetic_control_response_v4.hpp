#pragma once
#include <llvm/Support/JSON.h>
namespace pred::beir::arithmetic {
llvm::json::Object evaluateControlResponseV4(const llvm::json::Object& model,
    const llvm::json::Object& structure, bool diagnostic = false);
}
