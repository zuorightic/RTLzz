#pragma once
#include <map>
#include <string>
#include <stdexcept>

namespace rtlzz {
// Versioned core request. L includes the external output capture; RTLzz N=L-1.
// These are declarative settings, never permission to invoke a physical tool.
struct ArithmeticRequest {
    std::string version = "arithmetic-request-v1";
    std::string target_platform;
    std::string platform_profile;
    std::string platform_profile_sha256;
    std::string cost_mode = "fixed";
    std::string fixed_cost_table;
    std::string fixed_cost_table_sha256;
    std::string model_bundle;
    std::string model_bundle_sha256;
    std::string candidate_policy = "core-original-v1";
    std::string semantic_profile = "arithmetic-semantics-v2";
    int latency = 1;
    int initiation_interval = 1;
    double target_period_ps = 0;
    // Per-template parameters are validated by the registered implementation,
    // not silently ignored. Core original implementations need no parameters.
    std::map<std::string, int> template_parameters;
};
struct ArithmeticDiagnostic {
    std::string failure_stage, failure_code, failure_reason;
    std::string region_id;
    std::string node_id;
};
class ArithmeticError : public std::runtime_error {
public:
    ArithmeticDiagnostic diagnostic;
    explicit ArithmeticError(ArithmeticDiagnostic d)
        : std::runtime_error(d.failure_code+": "+d.failure_reason), diagnostic(std::move(d)) {}
};
ArithmeticRequest loadArithmeticRequest(const std::string& filename);
void validateArithmeticRequest(const ArithmeticRequest&, int register_stages);
std::string arithmeticDiagnosticJson(const ArithmeticDiagnostic&, const ArithmeticRequest* request = nullptr);
std::string arithmeticHash(const std::string& bytes);
}
