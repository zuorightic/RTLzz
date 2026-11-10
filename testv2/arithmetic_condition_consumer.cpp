#include "backend/arithmetic_condition_selection.hpp"
#include "backend/rtlgen.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>

using namespace pred;using namespace beir;using namespace beir::arithmetic;
using O=llvm::json::Object;using A=llvm::json::Array;using V=llvm::json::Value;
static void check(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
static std::string read(const std::filesystem::path& p){std::ifstream f(p);check(bool(f),"missing "+p.string());return {std::istreambuf_iterator<char>(f),{}};}
static V parse(const std::string& s){auto v=llvm::json::parse(s);if(!v)throw std::runtime_error("invalid JSON: "+llvm::toString(v.takeError()));return std::move(*v);}
static void write(const std::filesystem::path& p,V v){std::ofstream(p)<<llvm::formatv("{0:2}",v).str()<<'\n';}

// Software coefficients only. The actual projected state, cuts and edge sets
// come from the shared production library, never a MAC or view-ID lookup.
struct FixtureBoundary final : ConditionalBoundaryProvider {
    std::string corrupt;
    ConditionalBoundaryPrice price(const Program&,const StateCostQuery& state,
        const std::vector<ConditionalBoundaryRequest>& requests,const std::string& parent) const override {
        ConditionalBoundaryPrice b;b.covered=true;b.model_sha256=rtlzz::arithmeticHash("TEST_ONLY-state-and-boundary-v1");
        b.operator_model_sha256=parent;b.layout_sha256=state.layout_sha256;b.target_period_ps=state.period_ps;
        double native=0,external=0,nwords=0,ewords=0;
        for(const auto& slot:state.slots)if(slot.external_capture){external+=slot.variable_bits;++ewords;}else{native+=slot.variable_bits;++nwords;}
        b.owner_area_um2={{"input_service",.2*state.input_variable_bits},{"native_state",.2*native+.03*nwords},
            {"external_state",.18*external+.02*ewords},{"shared_clock",.3+.005*(native+external)},
            {"shared_physical",.1},{"root_repair",.05}};
        for(const auto& q:requests){ConditionalStageBoundary stage;stage.cut_sha256=q.cut.cut_sha256;
            stage.other_timing_violation_ps=0.;
            for(auto id:q.launches)stage.launch_arrival_ps[id]=20.+(q.cut.index==0?.2*state.period_ps:0.);
            for(auto id:q.captures)stage.capture_required_ps[id]=state.period_ps-20.;
            for(auto edge:q.edges)stage.wire_delay_ps[edge]=.5;
            b.stages.push_back(std::move(stage));}
        if(corrupt=="parent")b.operator_model_sha256=std::string(64,'0');
        if(corrupt=="layout")b.layout_sha256=std::string(64,'0');
        if(corrupt=="target")++b.target_period_ps;
        if(corrupt=="owner")b.owner_area_um2["operators"]=1.;
        if(corrupt=="nan")b.owner_area_um2["shared_clock"]=std::numeric_limits<double>::quiet_NaN();
        if(corrupt=="capture")for(auto& s:b.stages)if(!s.capture_required_ps.empty()){s.capture_required_ps.erase(s.capture_required_ps.begin());break;}
        if(corrupt=="wire")for(auto& s:b.stages)if(!s.wire_delay_ps.empty()){s.wire_delay_ps.erase(s.wire_delay_ps.begin());break;}
        if(corrupt=="other_timing")b.stages.front().other_timing_violation_ps=std::numeric_limits<double>::quiet_NaN();
        return b;
    }
};
int main(int argc,char** argv){try{
    check(argc==5,"root, source request, model fixture and output required");std::filesystem::path root=argv[1],out=argv[4];
    check(!std::filesystem::exists(out),"preserve earlier attempts");std::filesystem::create_directories(out);
    auto document=parse(read(argv[2]));const auto& row=*document.getAsObject()->getObject("source");
    pipelinev2::PipelineConfig c;c.source_name=row.getString("source_label")->str();c.source_text=read(row.getString("source")->str());
    check(rtlzz::arithmeticHash(*c.source_text)==row.getString("generated_source_sha256")->str(),"source hash drift");
    c.top_function=row.getString("top")->str();c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.pipeline.emplace();
    auto& options=*c.pipeline;options.register_stages=*row.getInteger("N");options.reset_enabled=true;
    options.input_valid_name=row.getString("iv")->str();options.output_valid_name=row.getString("ov")->str();
    for(const auto& item:*row.getObject("reset")){std::vector<std::uint64_t> words;for(const auto& v:*item.second.getAsArray())words.push_back(std::stoull(v.getAsString()->str(),nullptr,16));options.input_reset_values[item.first.str()]=words;}
    rtlzz::ArithmeticRequest fixed;fixed.version="arithmetic-request-v2";fixed.operator_boundary_policy="unsigned-op-hierarchy-v1";
    fixed.target_platform="asap7";fixed.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();fixed.platform_profile_sha256=rtlzz::arithmeticHash(read(fixed.platform_profile));
    fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));
    fixed.latency=options.register_stages+1;fixed.target_period_ps=1500;fixed.candidate_policy="core-original-v1";c.arithmetic_request=fixed;
    auto compiled=pipelinev2::compile(c);check(compiled.ok()&&compiled.pipeline_program.has_value(),compiled.error);
    auto source=compiled.pipeline_program->combinational;options.arithmetic_request=fixed;fixed.candidate_policy="mul-candidates-v1";
    auto inventory=enumerateConditionalViews(source,options,fixed);check(inventory.complete&&inventory.views.size()==44&&inventory.structures==3,"native structural inventory differs");
    auto model=fixed;model.cost_mode="backend-model";model.model_bundle=argv[3];model.model_bundle_sha256=rtlzz::arithmeticHash(read(argv[3]));
    FixtureBoundary boundary;A runs,negatives;std::size_t query_count=0;
    for(double target:{50.,150.,1500.}){
        model.target_period_ps=target;auto selected=selectConditionalViews(inventory,options,model,boundary,ConditionalUse::TestOnly);
        auto report=parse(selected.report);check(selected.pipeline.has_value(),"complete fixture did not select");
        check(report.getAsObject()->getBoolean("formal_D_selection")==false,"fixture granted production qualification");
        for(const auto& row:*report.getAsObject()->getArray("candidates"))query_count+=row.getAsObject()->getArray("node_prices")->size();
        auto filename="selected-"+std::to_string(int(target))+".sv";
        std::ofstream(out/filename)<<rtlgen::emitSystemVerilogWithArithmeticBoundaries(*selected.pipeline);
        (*report.getAsObject())["rtl_file"]=filename;(*report.getAsObject())["rtl_sha256"]=rtlzz::arithmeticHash(read(out/filename));runs.push_back(std::move(report));
    }
    auto reject=[&](const std::string& name,const std::function<void()>& body){bool caught=false;try{body();}catch(const std::exception&){caught=true;}check(caught,"negative accepted: "+name);negatives.push_back(name);};
    reject("candidate_in_production",[&]{rtlzz::validateArithmeticRequest(model,options.register_stages);});
    reject("candidate_factory",[&]{makeCostProvider(inventory.views.front().graph,model);});
    auto bad=model;bad.model_bundle_sha256=std::string(64,'0');reject("wrong_hash",[&]{ConditionalOperatorCostProvider p(inventory.views.front().graph,bad,ConditionalUse::TestOnly);});
    bad=model;bad.operator_boundary_policy="wrong";reject("wrong_boundary",[&]{ConditionalOperatorCostProvider p(inventory.views.front().graph,bad,ConditionalUse::TestOnly);});
    for(const auto& name:{"parent","layout","target","owner","nan","capture","wire","other_timing"}){
        boundary.corrupt=name;auto selected=selectConditionalViews(inventory,options,model,boundary,ConditionalUse::TestOnly);
        std::ofstream(out/(std::string("negative-")+name+".json"))<<selected.report;
        check(!selected.pipeline&&parse(selected.report).getAsObject()->getBoolean("coverage_complete")==false,"corrupt boundary selected RTL");negatives.push_back(name);
    }boundary.corrupt.clear();
    auto partial_model=parse(read(argv[3]));auto* domain=partial_model.getAsObject()->getObject("operations")->getObject("mul")->getObject("domain")->getObject("numeric_support");
    (*domain)["lhs_width"]=A{1,16};write(out/"partial-node-fixture.json",std::move(partial_model));
    auto partial_request=model;partial_request.model_bundle=(out/"partial-node-fixture.json").string();partial_request.model_bundle_sha256=rtlzz::arithmeticHash(read(partial_request.model_bundle));
    auto partial=selectConditionalViews(inventory,options,partial_request,boundary,ConditionalUse::TestOnly);auto partial_report=parse(partial.report);unsigned covered=0;
    for(const auto& v:*partial_report.getAsObject()->getArray("candidates"))covered+=v.getAsObject()->getString("status")=="covered";
    check(!partial.pipeline&&covered>0&&covered<inventory.views.size(),"partial model silently selected a supported subset");
    negatives.push_back("partial_node_coverage");write(out/"partial-coverage.json",std::move(partial_report));
    auto truncated=enumerateConditionalViews(source,options,fixed,1);check(!truncated.complete,"truncated inventory declared complete");
    check(!selectConditionalViews(truncated,options,model,boundary,ConditionalUse::TestOnly).pipeline,"truncated inventory selected");negatives.push_back("incomplete_inventory");
    auto changed=inventory;changed.views.pop_back();reject("changed_inventory",[&]{selectConditionalViews(changed,options,model,boundary,ConditionalUse::TestOnly);});
    // A formal-schema software fixture verifies factory dispatch and the
    // separate joint gate; these headers are not physical evidence.
    auto formal=parse(read(argv[3]));auto& b=*formal.getAsObject();b["quality_status"]="isolated_functions_passed";b.erase("test_only");
    for(auto& item:*b.getObject("operations"))(*item.second.getAsObject())["status"]="passed_independent";
    write(out/"formal-schema-fixture.json",std::move(formal));auto r=model;r.model_bundle=(out/"formal-schema-fixture.json").string();r.model_bundle_sha256=rtlzz::arithmeticHash(read(r.model_bundle));
    rtlzz::validateArithmeticRequest(r,options.register_stages);auto provider=makeCostProvider(inventory.views.front().graph,r);
    check(dynamic_cast<ConditionalOperatorCostProvider*>(provider.get()),"factory selected another schema");
    reject("node_only_pipeline",[&]{auto full=options;full.arithmetic_request=r;pipeline::build(inventory.views.front().graph,full);});
    auto production=selectConditionalViews(inventory,options,r,boundary);
    check(!production.pipeline&&parse(production.report).getAsObject()->getBoolean("formal_D_selection")==false,"unqualified joint boundary entered production");negatives.push_back("unqualified_joint_price");
    // Mutate a cut without changing its fingerprint; only an arithmetic member
    // is used so wiring cannot bypass stage-context verification.
    const auto& first=inventory.views.front();ConditionalOperatorCostProvider direct(first.graph,model,ConditionalUse::TestOnly);
    auto queries=direct.placementPrices(first.graph,first.placement,options.register_stages,{});
    if(!queries.empty()){
        auto actual=stageCostQuery(first.graph,first.placement,queries.front().query.stage,options.register_stages,{});
        actual.members.clear();reject("forged_cut_members",[&]{direct.nodeInStage(first.graph,queries.front().query.origin,actual);});
    }
    write(out/"consumer.json",O{{"version","conditional-consumer-software-proof-v1"},{"mode","TEST_ONLY"},{"source",O(row)},
        {"views",std::uint64_t(inventory.views.size())},{"assignments",std::uint64_t(inventory.assignments)},{"query_count",std::uint64_t(query_count)},
        {"runs",std::move(runs)},{"negative_checks",std::move(negatives)},{"candidate_PPA_calls",0},{"physical_labels_read",false},{"full_D_ready",false}});
    std::cout<<"CONDITIONAL_CONSUMER_PASS views="<<inventory.views.size()<<" queries="<<query_count<<" TEST_ONLY\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
