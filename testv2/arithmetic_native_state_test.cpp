#include "backend/arithmetic_state.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <set>

using namespace pred;
using namespace beir::arithmetic;
using O=llvm::json::Object;
static std::string read(const std::filesystem::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
static void check(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
int main(int argc,char** argv){try{
    check(argc==4,"root, manifest, output required");
    const std::filesystem::path root=argv[1],manifest=argv[2],out=argv[3];std::filesystem::create_directories(out);
    auto input=llvm::json::parse(read(manifest));check(bool(input),"state fixture manifest JSON");
    const auto* cases=input->getAsObject()->getArray("cases");check(cases,"state cases required");
    llvm::json::Array rows,rejections;
    for(const auto& item:*cases){const auto& c=*item.getAsObject();auto name=c.getString("name")->str();
        pipelinev2::PipelineConfig config;config.source_name=c.getString("source")->str();config.top_function=c.getString("top")->str();
        config.clang_args={"-std=c++20","-I"+(root/"vullib").string()};config.use_circt=false;
        config.pipeline.emplace();auto& options=*config.pipeline;
        options.register_stages=*c.getInteger("N");options.reset_enabled=true;
        options.input_valid_name=c.getString("iv")->str();options.output_valid_name=c.getString("ov")->str();
        for(const auto& entry:*c.getObject("reset")){
            std::vector<std::uint64_t> words;
            for(const auto& word:*entry.second.getAsArray())words.push_back(std::stoull(word.getAsString()->str(),nullptr,16));
            options.input_reset_values[entry.first.str()]=words;
        }
        rtlzz::ArithmeticRequest request;request.target_platform="asap7";
        request.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();
        request.platform_profile_sha256=rtlzz::arithmeticHash(read(request.platform_profile));
        request.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();
        request.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(request.fixed_cost_table));
        request.latency=options.register_stages+1;request.candidate_policy="core-original-v1";
        request.target_period_ps=1500;
        config.arithmetic_request=request;
        auto compiled=pipelinev2::compile(config);
        check(compiled.ok()&&compiled.pipeline_program.has_value(),name+": "+compiled.error);
        const auto& actual=*compiled.pipeline_program;
        auto projected=stateCostQuery(actual.combinational,actual.options,actual.stages,1500,true);
        auto bound=stateCostQuery(actual,1500,true);
        check(projected.features==bound.features&&projected.layout_sha256==bound.layout_sha256,name+": projection/materialization mismatch");
        auto parsed=llvm::json::parse(stateCostQueryJson(bound));auto row=std::move(*parsed->getAsObject());row["case"]=name;
        row["source_sha256"]=rtlzz::arithmeticHash(read(config.source_name));
        row["internal_registers"]=static_cast<std::uint64_t>(actual.registers.size());
        row["internal_register_bits"]=actual.report.register_bits;
        rows.push_back(std::move(row));
        std::ofstream(out/(name+".sv"))<<compiled.output_text;
        std::ofstream(out/(name+".beir"))<<beir::emitText(actual.combinational);
        if(name=="two_mux"){
            auto rejected=[&](const std::string& label,auto action){bool failed=false;std::string why;
                try{action();}catch(const std::exception& e){failed=true;why=e.what();}
                check(failed,label+" was accepted");rejections.push_back(O{{"test",label},{"reason",why}});};
            auto bad=actual;auto reg=std::find_if(bad.registers.begin(),bad.registers.end(),[](const auto& r){return !r.is_valid;});
            check(reg!=bad.registers.end(),"state negative needs a materialized data register");
            reg->enable=bad.valid_nodes.back();rejected("wrong-enable",[&]{stateCostQuery(bad,1500,true);});
            bad=actual;bad.registers.front().reset_values.front()^=1;rejected("wrong-reset",[&]{stateCostQuery(bad,1500,true);});
            bad=actual;bad.registers.push_back(bad.registers.front());rejected("duplicate-register",[&]{stateCostQuery(bad,1500,true);});
            auto changed=actual.stages;changed.front()=1;rejected("input-moved",[&]{stateCostQuery(actual.combinational,actual.options,changed,1500,true);});
            rejected("nonfinite-target",[&]{stateCostQuery(actual,0,true);});
            auto without_output=stateCostQuery(actual,1500,false);
            check(without_output.features.at("state_bits")<bound.features.at("state_bits"),"external capture ownership not explicit");
        }
    }
    std::ofstream(out/"queries.json")<<llvm::formatv("{0:2}",llvm::json::Value(O{{"version","native-state-binding-audit-v1"},
        {"rows",std::move(rows)},{"rejections",std::move(rejections)},{"PPA_calls",0},
        {"qualified_state_price",false},{"full_D_ready",false}})).str();
    std::cout<<"PASS materialized native state features, resets, enables, external capture ownership and refusal checks\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
