#pragma once
#include "backend/arithmetic_core.hpp"
#include "backend/pipeline.hpp"

namespace pred::beir::arithmetic {
struct StateSlot {
    NodeId origin = kInvalidNodeId;
    int stage = 0, enable_stage = -1;
    int declared_width = 0;
    std::uint64_t variable_bits = 0;
    bool valid = false, external_capture = false;
    std::string name;
    std::vector<std::uint64_t> variable_mask, reset_values;
};
struct StateCostQuery {
    std::string graph_sha256, layout_sha256, valid_input_rtl_name;
    double period_ps = 0;
    int internal_stages = 0;
    bool materialized_checked = false, external_outputs_included = false;
    std::vector<StateSlot> slots;
    std::map<std::string,double> features;
    std::uint64_t input_variable_bits = 0, control_reset_one_bits = 0;
    // Retain the projection inputs so a consumer can rebuild the query instead
    // of trusting caller-supplied feature counts, masks or layout fingerprints.
    pipeline::Options projection_options;
    std::vector<int> placement;
};
// External output capture is explicit: the RTLzz core does not emit it.
// Input/service state outside the core is a separate accounting owner.
StateCostQuery stateCostQuery(const Program&,const pipeline::Options&,
                             const std::vector<int>& placement,double period_ps,
                             bool external_outputs);
// Also checks the actual materialized registers, source alignment, enables,
// reset values, and projection before returning the same semantic layout.
StateCostQuery stateCostQuery(const pipeline::Program&,double period_ps,bool external_outputs);
std::string stateCostQueryJson(const StateCostQuery&);
}
