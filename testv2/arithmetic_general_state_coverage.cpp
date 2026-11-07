#include "backend/arithmetic_general_state.hpp"
#include "backend/arithmetic_selection.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static std::string read(const std::filesystem::path& p){std::ifstream f(p);if(!f)throw std::runtime_error("read "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
static V parse(const std::string& s){auto v=llvm::json::parse(s);if(!v)throw std::runtime_error("JSON parse");return std::move(*v);}
static void check(bool x,const std::string& why){if(!x)throw std::runtime_error(why);}
static void write(const std::filesystem::path& p,V v){std::ofstream(p)<<llvm::formatv("{0:2}",v).str()<<'\n';}
int main(int argc,char** argv){try{
    check(argc==6,"root, public cases, general manifest, old static audit native dir, out required");
    const std::filesystem::path root=argv[1],cases=argv[2],manifest=argv[3],audit=argv[4],out=argv[5];std::filesystem::create_directories(out);
    auto entries=parse(read(cases)),old_index=parse(read(audit/"index.json"));auto targets=parse(read(root/"experiments/arithmetic_dse/configs/model_asap7_m2_targets_v1.json"));
    rtlzz::ArithmeticRequest fixed;fixed.cost_mode="fixed";fixed.target_platform="asap7";fixed.target_period_ps=1500;
    fixed.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
    fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));
    A rows;int candidates=0,views=0,actual_checks=0;
    for(const auto& value:*entries.getAsObject()->getArray("cases")){
        const auto& entry=*value.getAsObject();auto name=entry.getString("name")->str();const O* previous=nullptr;
        for(const auto& v:*old_index.getAsObject()->getArray("cases"))if(v.getAsObject()->getString("case")==name)previous=v.getAsObject();
        check(previous,"public case in old static audit");
        pipelinev2::PipelineConfig c;c.source_name=entry.getString("source_label")->str();c.source_text=read(entry.getString("source")->str());c.top_function=entry.getString("top")->str();
        c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.pipeline.emplace();auto& options=*c.pipeline;
        options.register_stages=*entry.getInteger("N");options.reset_enabled=true;options.input_valid_name=entry.getString("iv")->str();options.output_valid_name=entry.getString("ov")->str();
        for(const auto& v:*entry.getObject("reset")){std::vector<std::uint64_t> words;for(const auto& w:*v.second.getAsArray())words.push_back(std::stoull(w.getAsString()->str(),nullptr,16));options.input_reset_values[v.first.str()]=words;}
        auto request=fixed;request.latency=options.register_stages+1;request.candidate_policy="core-original-v1";c.arithmetic_request=request;
        auto compiled=pipelinev2::compile(c);check(compiled.ok()&&compiled.pipeline_program.has_value(),name+": "+compiled.error);
        auto graph=compiled.pipeline_program->combinational;request.candidate_policy="mul-candidates-v1";
        auto enumeration=visitCandidateGraphs(graph,request,[&](const Program& candidate,const std::string& id,const std::string& shape){
            const O* prior=nullptr;for(const auto& v:*previous->getArray("candidates"))if(v.getAsObject()->getString("id")==id)prior=v.getAsObject();
            check(prior&&prior->getString("shape_sha256")==shape,"production candidate order/shape unchanged");
            auto old_path=audit/prior->getString("file")->str();check(rtlzz::arithmeticHash(read(old_path))==prior->getString("sha256"),"old frozen candidate audit identity");
            auto old=parse(read(old_path));auto settings=options;settings.arithmetic_request=request;auto actual=pipeline::build(candidate,settings);
            check(rtlzz::arithmeticHash(emitText(actual.combinational))==old.getAsObject()->getString("materialized_graph_sha256"),"materialized graph identity unchanged");
            auto target=targets.getAsObject()->getObject("cases")->getObject(name);
            check(target,"case target freeze");auto period=target->getNumber("T_target_ps");check(bool(period),"formal exact target");
            auto r=request;r.cost_mode="backend-model";r.target_period_ps=*period;r.fixed_cost_table.clear();r.fixed_cost_table_sha256.clear();r.model_bundle=manifest.string();r.model_bundle_sha256=rtlzz::arithmeticHash(read(manifest));
            auto provider=makeCostProvider(actual.combinational,r);
            ServiceStateProtocol protocol;protocol.mode=1;protocol.valid_write_enable="push__vld__";protocol.external_data_unconditional=protocol.external_valid_unconditional=true;
            for(const auto& p:actual.combinational.ports)if(p.direction==PortDirection::Input&&p.name!=settings.input_valid_name)protocol.data_write_enables[p.name]="push__vld__";
            auto final=generalStateCostQuery(actual,*period,protocol);++actual_checks;A projected;bool final_seen=false;
            for(const auto& view:*old.getAsObject()->getArray("views")){
                std::vector<int> placement;for(const auto& v:*view.getAsObject()->getArray("placement"))placement.push_back(*v.getAsInteger());
                auto query=generalStateCostQuery(actual.combinational,settings,placement,*period,protocol);
                if(placement==actual.stages){check(query.layout_sha256==final.layout_sha256,"final actual/projected general state matches");final_seen=true;}
                A roles;for(const auto& role:*view.getAsObject()->getArray("roles"))roles.push_back(role.getAsString()->str());
                O row{{"query",parse(generalStateCostQueryJson(query))},{"roles",std::move(roles)}};
                try{row["price"]=parse(generalStateCostEstimateJson(provider->generalStateCost(actual.combinational,query)));row["status"]="predicted_in_domain";}
                catch(const rtlzz::ArithmeticError& e){row["status"]="cost_uncovered";row["reason"]=e.diagnostic.failure_reason;}
                projected.push_back(std::move(row));++views;
            }
            check(final_seen,"final materialized map in old view inventory");
            write(out/(name+"-"+std::to_string(candidates)+".json"),O{{"case",name},{"candidate",id},{"shape_sha256",shape},
                {"target_period_ps",*period},{"actual_final_checked",true},{"views",std::move(projected)},{"PPA_calls",0},{"full_D_ready",false}});
            rows.push_back(O{{"case",name},{"candidate",id},{"shape_sha256",shape}});++candidates;
        });
        check(enumeration.unique==previous->getInteger("unique"),"production candidate count unchanged");
    }
    check(candidates==21&&views==108&&actual_checks==21,"complete previous public static view coverage");
    write(out/"index.json",O{{"version","general-state-all-candidate-coverage-v1"},{"candidate_count",candidates},{"view_count",views},
        {"actual_final_checks",actual_checks},{"rows",std::move(rows)},{"K",16},{"candidate_order_unchanged",true},
        {"old_static_views_replayed",true},{"future_D_move_enumeration_claimed",false},{"PPA_calls",0},{"full_D_ready",false}});
    std::cout<<"PASS nine cases /21 ordered candidates /108 static views /21 actual final materializations\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
