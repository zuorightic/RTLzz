#include "rtlzz.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>
static std::string read(const std::filesystem::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
static void check(bool ok,const char* reason){if(!ok)throw std::runtime_error(reason);}
int main(int argc,char**argv){try{
    check(argc==3,"root and new output required");std::filesystem::path root=argv[1],out=argv[2];check(!std::filesystem::exists(out),"preserve attempts");std::filesystem::create_directories(out);
    rtlzz::ArithmeticRequest r;r.version="arithmetic-request-v2";r.operator_boundary_policy="unsigned-op-hierarchy-v1";r.target_platform="asap7";
    r.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();r.platform_profile_sha256=rtlzz::arithmeticHash(read(r.platform_profile));r.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();r.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(r.fixed_cost_table));r.latency=2;r.target_period_ps=1500;
    rtlzz::CompileOptions o;o.arithmetic_request=r;o.vullib_dir=(root/"vullib").string();o.top_function="top";o.rtl_module_body=true;o.register_stages=1;o.reset_enabled=true;o.input_valid_name="iv";o.output_valid_name="ov";
    o.input_reset_values={{"a",{0}},{"b",{0}},{"iv",{0}}};
    o.source_codelines={"#include <fixint.hpp>\nInt<12>a;Int<20>b;Int<32>y;bool iv,ov;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port ov\nvoid top(){y=Int<32>(a)*Int<32>(b);}"};
    o.rtl_port_bindings={{"a","a"},{"b","b"},{"iv","iv"},{"y","y"},{"ov","ov"},{"clk","clk"},{"rstn","rstn"}};
    auto result=rtlzz::compileToRtl(o);check(result.ok(),result.error.c_str());check(!result.rtl_module_definitions.empty(),"missing file-scope modules");std::string body;for(const auto& s:result.output_codelines)body+=s;
    auto parsed=llvm::json::parse(result.arithmetic_report);check(bool(parsed),"report parse");auto* report=parsed->getAsObject();check(report->getString("rtl_sha256")==rtlzz::arithmeticHash(body+result.rtl_module_definitions),"combined body/modules identity mismatch");
    auto legacy=o;legacy.arithmetic_request->version="arithmetic-request-v1";legacy.arithmetic_request->operator_boundary_policy.clear();auto old=rtlzz::compileToRtl(legacy);check(old.ok()&&old.rtl_module_definitions.empty(),"legacy native path changed");
    auto invalid=o;invalid.arithmetic_request->version="arithmetic-request-v1";check(!rtlzz::compileToRtl(invalid).ok(),"v1 accepted v2 policy");
    invalid=o;invalid.arithmetic_request->operator_boundary_policy.clear();check(!rtlzz::compileToRtl(invalid).ok(),"v2 accepted no boundary");
    invalid=o;invalid.use_circt=true;auto rejected=rtlzz::compileToRtl(invalid);check(!rejected.ok()&&rejected.arithmetic_diagnostic&&rejected.arithmetic_diagnostic->failure_code=="unsupported_backend_op","CIRCT silently dropped boundary contract");
    invalid=o;invalid.arithmetic_request->cost_mode="backend-model";invalid.arithmetic_request->fixed_cost_table.clear();invalid.arithmetic_request->fixed_cost_table_sha256.clear();invalid.arithmetic_request->model_bundle=(root/"experiments/arithmetic/evidence/step7_m2_mux_static_v1/asap7-primitives.json").string();invalid.arithmetic_request->model_bundle_sha256=rtlzz::arithmeticHash(read(invalid.arithmetic_request->model_bundle));
    rejected=rtlzz::compileToRtl(invalid);check(!rejected.ok()&&rejected.arithmetic_diagnostic&&rejected.arithmetic_diagnostic->failure_code=="cost_uncovered","old model silently inherited new boundary");
    std::ofstream(out/"body.sv")<<body;std::ofstream(out/"modules.sv")<<result.rtl_module_definitions;std::ofstream(out/"report.json")<<result.arithmetic_report;
    std::ofstream(out/"proof.json")<<"{\"status\":\"passed\",\"v1_preserved\":true,\"v2_body_definitions_bound\":true,\"boundary_mismatch_rejections\":4,\"production_D_qualified\":false}\n";
    std::cout<<"BOUNDARY_REQUEST_V2_PASS legacy=1 body_identity=1 refusals=4\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
