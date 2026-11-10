#include "backend/arithmetic_state_boundary.hpp"
#include "backend/rtlgen.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>

using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static void check(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
static std::string read(const std::filesystem::path& p){std::ifstream f(p);check(bool(f),"missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
static V parse(const std::string& s){auto v=llvm::json::parse(s);if(!v)throw std::runtime_error(llvm::toString(v.takeError()));return std::move(*v);}
static void write(const std::filesystem::path& p,V v){std::ofstream(p)<<llvm::formatv("{0:2}",v).str()<<'\n';}
int main(int argc,char** argv){try{
    check(argc==6,"root, source request, node candidate, real S binding, output required");std::filesystem::path root=argv[1],out=argv[5];check(!std::filesystem::exists(out),"preserve previous attempts");std::filesystem::create_directories(out);
    auto document=parse(read(argv[2]));const auto& row=*document.getAsObject()->getObject("source");pipelinev2::PipelineConfig c;
    c.source_name=row.getString("source_label")->str();c.source_text=read(row.getString("source")->str());check(rtlzz::arithmeticHash(*c.source_text)==row.getString("generated_source_sha256")->str(),"public source changed");
    c.top_function=row.getString("top")->str();c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.pipeline.emplace();auto& options=*c.pipeline;
    options.register_stages=*row.getInteger("N");options.reset_enabled=true;options.input_valid_name=row.getString("iv")->str();options.output_valid_name=row.getString("ov")->str();
    for(const auto& item:*row.getObject("reset")){std::vector<std::uint64_t> words;for(const auto& v:*item.second.getAsArray())words.push_back(std::stoull(v.getAsString()->str(),nullptr,16));options.input_reset_values[item.first.str()]=words;}
    rtlzz::ArithmeticRequest fixed;fixed.version="arithmetic-request-v2";fixed.operator_boundary_policy="unsigned-op-hierarchy-v1";fixed.target_platform="asap7";
    fixed.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
    fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));fixed.latency=options.register_stages+1;fixed.target_period_ps=1500;
    c.arithmetic_request=fixed;auto compiled=pipelinev2::compile(c);check(compiled.ok()&&compiled.pipeline_program.has_value(),compiled.error);
    options.arithmetic_request=fixed;fixed.candidate_policy="mul-candidates-v1";auto inventory=enumerateConditionalViews(compiled.pipeline_program->combinational,options,fixed);check(inventory.complete&&inventory.views.size()==44,"structural views changed");
    auto model=fixed;model.cost_mode="backend-model";model.model_bundle=argv[3];model.model_bundle_sha256=rtlzz::arithmeticHash(read(argv[3]));std::string binding=argv[4],binding_sha=rtlzz::arithmeticHash(read(binding));
    A runs,negatives;
    for(double target:{500.,1500.,3000.}){model.target_period_ps=target;ReusedStateBoundaryProvider provider(binding,binding_sha,model,StateBoundaryUse::CandidateReuse);
        auto selected=selectConditionalViews(inventory,options,model,provider,ConditionalUse::TestOnly);auto report=parse(selected.report);
        check(selected.pipeline.has_value(),"real S candidate has incomplete coverage: "+selected.report.substr(0,4000));
        check(report.getAsObject()->getBoolean("formal_D_selection")==false,"candidate reuse granted production");
        for(const auto& row:*report.getAsObject()->getArray("candidates")){
            const auto& value=*row.getAsObject();check(value.getString("state_model_sha256").has_value(),"S identity not reported");
            check(value.getObject("boundary_diagnostics")->getBoolean("S_coefficients_changed")==false,"S coefficients changed");
        }
        auto name="selected-"+std::to_string(int(target))+".sv";std::ofstream(out/name)<<rtlgen::emitSystemVerilogWithArithmeticBoundaries(*selected.pipeline);
        (*report.getAsObject())["rtl_file"]=name;(*report.getAsObject())["rtl_sha256"]=rtlzz::arithmeticHash(read(out/name));runs.push_back(std::move(report));
    }
    model.target_period_ps=1500;const auto& view=inventory.views.back();auto actual=pipeline::materializeSchedule(view.graph,options,view.placement);auto q=stateCostQuery(actual,1500,true);
    std::vector<NodeId> controls;for(const auto& name:{options.input_valid_name,options.output_valid_name})for(const auto& port:actual.combinational.ports)if(port.name==name)controls.push_back(port.element_nodes.front());
    auto requests=conditionalBoundaryRequests(actual.combinational,view.placement,options.register_stages,controls);
    ReusedStateBoundaryProvider provider(binding,binding_sha,model,StateBoundaryUse::CandidateReuse);
    auto reject=[&](const std::string& name,const std::function<void(StateCostQuery&,std::vector<ConditionalBoundaryRequest>&,std::string&)>& change){auto bad=q;auto cuts=requests;auto parent=model.model_bundle_sha256;change(bad,cuts,parent);auto price=provider.price(actual.combinational,bad,cuts,parent);check(!price.covered,"invalid state reuse accepted: "+name);negatives.push_back(O{{"test",name},{"reason",price.reason}});};
    reject("forged_features",[](auto& q,auto&,auto&){q.features["data_bits"]+=1;});
    reject("forged_mask",[](auto& q,auto&,auto&){q.slots.front().variable_mask.front()^=1;});
    reject("forged_reset",[](auto& q,auto&,auto&){q.slots.front().reset_values.front()^=1;});
    reject("forged_enable",[](auto& q,auto&,auto&){++q.slots.front().enable_stage;});
    reject("unmaterialized",[](auto& q,auto&,auto&){q.materialized_checked=false;});
    reject("forged_internal_depth",[](auto& q,auto&,auto&){--q.internal_stages;});
    reject("forged_valid_identity",[](auto& q,auto&,auto&){q.valid_input_rtl_name="other_valid";});
    reject("wrong_target",[](auto& q,auto&,auto&){q.period_ps=1000;});
    reject("wrong_node_dependency",[](auto&,auto&,auto& p){p=std::string(64,'0');});
    reject("missing_wire",[](auto&,auto& cuts,auto&){for(auto& c:cuts)if(!c.edges.empty()){c.edges.erase(c.edges.begin());break;}});
    reject("missing_capture",[](auto&,auto& cuts,auto&){for(auto& c:cuts)if(!c.captures.empty()){c.captures.erase(c.captures.begin());break;}});
    reject("missing_stage",[](auto&,auto& cuts,auto&){cuts.pop_back();});
    ReusedStateBoundaryProvider qualified(binding,binding_sha,model);check(!qualified.price(actual.combinational,q,requests,model.model_bundle_sha256).covered,"candidate S reuse accepted as qualified");negatives.push_back(O{{"test","unqualified_production_reuse"},{"reason","qualified mode refuses candidate flow/joint coverage"}});
    bool wrong=false;try{ReusedStateBoundaryProvider bad(binding,std::string(64,'0'),model,StateBoundaryUse::CandidateReuse);}catch(const std::exception&){wrong=true;}check(wrong,"wrong binding hash accepted");negatives.push_back(O{{"test","wrong_binding_hash"}});
    auto reject_binding=[&](const std::string& name,const std::function<void(O&)>& change){
        auto modified=parse(read(binding));auto& b=*modified.getAsObject();
        for(const auto* key:{"state_model","service_contract"}){
            auto& entry=*b.getObject(key);auto reference=std::filesystem::path(entry.getString("path")->str());
            entry["path"]=(reference.is_absolute()?reference:std::filesystem::path(binding).parent_path()/reference).lexically_normal().string();
        }
        change(b);auto path=out/(name+".json");write(path,std::move(modified));bool rejected=false;
        try{ReusedStateBoundaryProvider bad(path.string(),rtlzz::arithmeticHash(read(path)),model,StateBoundaryUse::CandidateReuse);}catch(const std::exception&){rejected=true;}
        check(rejected,"altered S dependency accepted: "+name);negatives.push_back(O{{"test",name}});
    };
    reject_binding("changed_control_parameter",[](auto& b){auto& m=*b.getObject("control_models")->getObject("native_enable");m["reserve_ps"]=*m.getNumber("reserve_ps")+1.;});
    reject_binding("changed_S_path_bound",[](auto& b){auto& six=*b.getObject("six_path_upper_ps");six["native_data"]=*six.getNumber("native_data")+1.;});
    reject_binding("changed_S_clock_inventory",[](auto& b){b.getArray("control_exact_clocks")->pop_back();});
    reject_binding("empty_service_proof",[](auto& b){(*b.getObject("service_contract"))["required_tokens"]=A{};});
    write(out/"real-boundary.json",O{{"version","real-state-boundary-consumer-v1"},{"mode","candidate_reuse"},{"views",44},{"runs",std::move(runs)},{"negative_checks",std::move(negatives)},
        {"binding_sha256",binding_sha},{"state_model_unchanged",true},{"old_test_labels_read",false},{"operator_physical_labels_read",false},{"full_D_ready",false}});
    std::cout<<"REAL_STATE_BOUNDARY_CONSUMER_PASS views=44 targets=3 no_TEST_ONLY_boundary_coefficients\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
