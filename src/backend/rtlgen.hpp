#pragma once

#include "backend/beir.hpp"
#include "backend/pipeline.hpp"
#include "backend/arithmetic_reduction.hpp"
#include "backend/arithmetic_workload_plan.hpp"

#include <optional>
#include <string>
#include <vector>
#include <utility>

namespace pred::rtlgen {

struct RtlDebugDerivedNode {
    beir::NodeId id = beir::kInvalidNodeId;
    std::string name;
};

struct RtlDebugSignal {
    beir::NodeId id = beir::kInvalidNodeId;
    std::string signal_name;
    int rtl_line = 0;
    std::string port_name;
    int port_element_index = -1;
    beir::ValueType type;
    std::optional<DebugLoc> decl_loc;
    std::vector<DebugLoc> primary_locs;
    std::vector<DebugLoc> related_locs;
    std::vector<std::string> messages;
    std::vector<RtlDebugDerivedNode> derived_nodes;
    std::vector<std::string> derived_names;
};

std::string emitSystemVerilog(const beir::Program& program);
std::string emitSystemVerilog(const pipeline::Program& program);
// Explicit unsigned Mul/Add/AddCarry module boundaries. The same entry is
// available for combinational A and materialized B/C/D. Flow compatibility and
// mapped boundary/area qualification remain separate experiment requirements.
std::string emitSystemVerilogWithArithmeticBoundaries(const pipeline::Program&);
std::string emitSystemVerilogWithArithmeticBoundaries(const beir::Program&);
// G successor: caller supplies the same pure-discovery regions used in pricing.
// Each region is emitted once, with no kept Add hierarchy inside it.
std::string emitSystemVerilogWithReductionBoundaries(const pipeline::Program&,
    const std::vector<beir::arithmetic::ReductionRegion>&);
std::string emitSystemVerilogWithWorkloadOwners(const pipeline::Program&,
    const beir::arithmetic::WorkloadPlan&);
struct ArithmeticBoundaryBody { std::string body, module_definitions; };
ArithmeticBoundaryBody emitSystemVerilogBodyWithArithmeticBoundaries(
    const pipeline::Program&,const std::vector<std::pair<std::string,std::string>>&);
ArithmeticBoundaryBody emitSystemVerilogBodyWithArithmeticBoundaries(
    const beir::Program&,const std::vector<std::pair<std::string,std::string>>&);
std::string emitSystemVerilogBody(
    const pipeline::Program& program,
    const std::vector<std::pair<std::string, std::string>>& port_bindings);
// Emit declarations and statements for insertion into an existing module.
// Each binding maps an RTLZZ port name to a signal in the containing module.
std::string emitSystemVerilogBody(
    const beir::Program& program,
    const std::vector<std::pair<std::string, std::string>>& port_bindings);
std::vector<RtlDebugSignal> collectDebugSignals(const beir::Program& program,
                                                const std::string& rtl_text);
std::string emitDebugReport(const beir::Program& program,
                            const std::string& rtl_text);

} // namespace pred::rtlgen
