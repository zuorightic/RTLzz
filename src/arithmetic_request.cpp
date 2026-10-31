#include "arithmetic_request.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/SHA256.h>
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <sstream>
#include <cmath>
#include <limits>
#include <set>
#include <filesystem>
namespace rtlzz {
namespace {
[[noreturn]] void bad(const std::string& code,const std::string& message) {
    throw ArithmeticError({code=="cost_uncovered"?"cost":code=="template_ineligible"?"eligibility":"request_validation",code,message,{},{}});
}
std::string read(const std::string& path) {
    std::ifstream in(path,std::ios::binary);if(!in)bad("invalid_request","cannot read "+path);
    std::ostringstream data;data<<in.rdbuf();return data.str();
}
llvm::json::Object object(const std::string& bytes) {
    auto parsed=llvm::json::parse(bytes);
    if(!parsed) {llvm::consumeError(parsed.takeError());bad("invalid_request","invalid JSON");}
    auto* result=parsed->getAsObject();if(!result)bad("invalid_request","JSON object required");
    return std::move(*result);
}
void keys(const llvm::json::Object& o,const std::set<std::string>& allowed) {
    for(const auto& item:o)if(!allowed.count(item.first.str()))bad("invalid_request","unknown request field: "+item.first.str());
}
std::string string(const llvm::json::Object& o,const char* key,bool required=true) {
    auto v=o.getString(key);if(!v) {if(required || o.get(key))bad("invalid_request",std::string("string required: ")+key);return {};}
    return v->str();
}
int integer(const llvm::json::Object& o,const char* key) {
    auto v=o.getInteger(key);if(!v || *v<1 || *v>std::numeric_limits<int>::max())bad("invalid_request",std::string("positive integer required: ")+key);return *v;
}
}
std::string arithmeticDiagnosticJson(const ArithmeticDiagnostic& d, const ArithmeticRequest* request) {
    llvm::json::Object o{{"version","arithmetic-structure-report-v2"},{"status","failed"},{"failure_stage",d.failure_stage},{"failure_code",d.failure_code},{"failure_reason",d.failure_reason},{"region_id",d.region_id},{"node_id",d.node_id}};
    if(request)o["request"]=llvm::json::Object{{"target_platform",request->target_platform},{"platform_profile_sha256",request->platform_profile_sha256},{"cost_mode",request->cost_mode},{"model_bundle_sha256",request->model_bundle_sha256},{"candidate_policy",request->candidate_policy},{"semantic_profile",request->semantic_profile},{"L",request->latency},{"II",request->initiation_interval},{"target_period_ps",request->target_period_ps}};
    return llvm::formatv("{0:2}",llvm::json::Value(std::move(o))).str()+"\n";
}
std::string arithmeticHash(const std::string& bytes) {
    auto hash=llvm::SHA256::hash(llvm::ArrayRef<std::uint8_t>(reinterpret_cast<const std::uint8_t*>(bytes.data()),bytes.size()));
    std::string out;for(auto b:hash){out+="0123456789abcdef"[b>>4];out+="0123456789abcdef"[b&15];}return out;
}
ArithmeticRequest loadArithmeticRequest(const std::string& filename) {
    auto o=object(read(filename));
    keys(o,{"version","target_platform","platform_profile","platform_profile_sha256","cost_mode","fixed_cost_table","fixed_cost_table_sha256","model_bundle","model_bundle_sha256","candidate_policy","semantic_profile","L","II","target_period_ps","template_parameters"});
    ArithmeticRequest r;r.version=string(o,"version");r.target_platform=string(o,"target_platform");
    r.platform_profile=string(o,"platform_profile");r.platform_profile_sha256=string(o,"platform_profile_sha256");
    r.cost_mode=string(o,"cost_mode");r.candidate_policy=string(o,"candidate_policy");r.semantic_profile=string(o,"semantic_profile");
    r.fixed_cost_table=string(o,"fixed_cost_table",false);r.fixed_cost_table_sha256=string(o,"fixed_cost_table_sha256",false);
    r.model_bundle=string(o,"model_bundle",false);r.model_bundle_sha256=string(o,"model_bundle_sha256",false);
    r.latency=integer(o,"L");r.initiation_interval=integer(o,"II");auto period=o.getNumber("target_period_ps");
    if(!period)bad("invalid_request","numeric target_period_ps required");r.target_period_ps=*period;
    if(auto* v=o.get("template_parameters")) {
        auto* p=v->getAsObject();if(!p)bad("invalid_request","template_parameters must be an object");
        for(const auto& entry:*p) {auto n=entry.second.getAsInteger();if(!n || *n<0 || *n>std::numeric_limits<int>::max())bad("invalid_request","integer template parameter required");r.template_parameters[entry.first.str()]=*n;}
    }
    // Resolve relative data paths against the request, not the process cwd.
    auto resolve=[&](std::string& path){if(!path.empty() && std::filesystem::path(path).is_relative())path=(std::filesystem::path(filename).parent_path()/path).lexically_normal().string();};
    resolve(r.platform_profile);resolve(r.fixed_cost_table);resolve(r.model_bundle);
    return r;
}
void validateArithmeticRequest(const ArithmeticRequest& r,int n) {
    if(r.version!="arithmetic-request-v1")bad("invalid_request","unsupported request version");
    if(r.target_platform!="asap7" && r.target_platform!="nangate45" && r.target_platform!="sky130hd" && r.target_platform!="gf180")bad("invalid_request","unsupported target platform");
    if(r.latency<1 || r.latency==std::numeric_limits<int>::max() || n!=r.latency-1)bad("invalid_request","request L must equal register_stages+1");
    if(r.initiation_interval!=1)bad("invalid_request","only II=1 is implemented");
    if(!std::isfinite(r.target_period_ps) || r.target_period_ps<=0)bad("invalid_request","target period must be finite and positive ps");
    if(r.semantic_profile!="arithmetic-semantics-v2")bad("template_ineligible","unregistered semantic profile: "+r.semantic_profile);
    if(r.candidate_policy!="core-original-v1" || !r.template_parameters.empty())bad("template_ineligible","step4 implements registered original templates; parameterized split policies are step5");
    auto profile=read(r.platform_profile);
    if(arithmeticHash(profile)!=r.platform_profile_sha256)bad("invalid_request","platform profile content identity mismatch");
    auto p=object(profile);
    if(string(p,"platform")!=r.target_platform || string(p,"schema")!="arithmetic-platform-v1")bad("invalid_request","wrong platform profile");
    if(r.cost_mode=="fixed") {
        if(!r.model_bundle.empty() || !r.model_bundle_sha256.empty())bad("invalid_request","fixed cost mode does not consume a model bundle");
        auto table=read(r.fixed_cost_table);
        if(r.fixed_cost_table_sha256!="d83c391e3b5435b40d9805c869b01cd9ff8cdf2dae27c173223ca4614d2c59bd" || arithmeticHash(table)!=r.fixed_cost_table_sha256)bad("invalid_request","fixed table content identity mismatch");
        auto cost=object(table);
        if(string(cost,"version")!="fixed-op-width-v1")bad("cost_uncovered","unsupported fixed cost version");
    } else if(r.cost_mode=="backend-model") {
        if(r.model_bundle.empty() || r.model_bundle_sha256.empty())bad("cost_uncovered","backend model bundle is required; no fixed-cost fallback");
        auto bytes=read(r.model_bundle);
        if(arithmeticHash(bytes)!=r.model_bundle_sha256)bad("cost_uncovered","model bundle content identity mismatch");
        auto model=object(bytes);
        if(string(model,"target_platform")!=r.target_platform || string(model,"platform_profile_sha256")!=r.platform_profile_sha256)bad("cost_uncovered","model platform/domain identity mismatch");
        bad("cost_uncovered","offline model inference is not implemented until step7");
    } else bad("invalid_request","unknown cost mode: "+r.cost_mode);
}
}
