#pragma once

#include "backend/beir.hpp"

#include <string>
#include <utility>
#include <vector>

namespace pred::pipeline { struct Program; }

namespace pred::circt {

struct Result {
    std::string verilog;
    std::string error;
    std::string input_mlir_path;
    std::string optimized_mlir_path;
    std::string stderr_path;

    bool ok() const { return error.empty(); }
};

// Lower a fully optimized BEIR graph into a combinational CIRCT HW/Comb
// module, run the local circt-opt pipeline, and return exported Verilog.  When
// module_body is true, the module wrapper is removed and ports are rebound to
// the containing RTL module using port_bindings.
Result emitSystemVerilog(const beir::Program& program,
                         bool module_body,
                         const std::vector<std::pair<std::string, std::string>>& port_bindings,
                         bool retain_intermediates);

// Lower the existing schedule and register table to HW/Comb/Seq, using the
// same tool invocation, output binding, diagnostics and cleanup as above.
Result emitSystemVerilog(const pipeline::Program& program,
                         bool module_body,
                         const std::vector<std::pair<std::string, std::string>>& port_bindings,
                         bool retain_intermediates);

} // namespace pred::circt
