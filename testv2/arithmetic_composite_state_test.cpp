#include "backend/arithmetic_composite.hpp"
#include "pipelinev2/PipelineV2.h"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <filesystem>
#include <algorithm>
#include <fstream>
#include <iostream>
#include <limits>

using namespace pred;
using namespace beir;
using namespace beir::arithmetic;
using O=llvm::json::Object;
using V=llvm::json::Value;
static std::string read(const std::filesystem::path& p){std::ifstream f(p);return {std::istreambuf_iterator<char>(f),{}};}
static void check(bool value,const std::string& why){if(!value)throw std::runtime_error(why);}
static NodeId add(Program& p,int width,OperationKind kind,std::vector<Operand> operands={},OpCode op=OpCode::None){
    Signal s;s.id=p.signals.size();s.name="n"+std::to_string(s.id);s.type.width=width;Operation d;d.kind=kind;d.type=s.type;d.op=op;d.operands=std::move(operands);s.driver=d;p.signals.push_back(s);return s.id;
}
static Operand ref(const Program& p,NodeId id){Operand a;a.kind=OperandKind::Symbol;a.node=id;a.type=p.signal(id).type;return a;}
static NodeId input(Program& p,int width,const std::string& name){Operand a;a.kind=OperandKind::Port;a.type.width=width;a.text=name;auto id=add(p,width,OperationKind::PortRead,{a});Port port;port.name=name;port.type.width=width;port.element_nodes={id};port.direction=PortDirection::Input;p.ports.push_back(port);p.inputs.push_back(name);return id;}
static NodeId output(Program& p,NodeId id,const std::string& name){auto n=add(p,p.signal(id).type.width,OperationKind::Assign,{ref(p,id)});Port port;port.name=name;port.type=p.signal(id).type;port.element_nodes={n};port.direction=PortDirection::Output;p.ports.push_back(port);p.outputs.push_back(name);return n;}
struct Fixture {Program p;pipeline::Options options;std::vector<int> placement;NodeId data,computation=kInvalidNodeId;};
static Fixture fixture(int width,int n,std::uint64_t reset=0,bool arithmetic=false) {
    Fixture f;f.p.function_name="NativeStateConsumerFixture";f.options.register_stages=n;f.options.reset_enabled=true;f.options.input_valid_name="iv";f.options.output_valid_name="ov";
    auto a=input(f.p,width,"a");f.data=a;
    if(arithmetic){auto b=input(f.p,width,"b");f.computation=add(f.p,width,OperationKind::Binary,{ref(f.p,a),ref(f.p,b)},OpCode::Add);f.data=f.computation;auto c=input(f.p,width,"c");output(f.p,c,"z");}
    auto iv=input(f.p,1,"iv");output(f.p,f.data,"y");output(f.p,iv,"ov");
    for(const auto& port:f.p.ports)if(port.direction==PortDirection::Input) {
        auto r=port.name=="iv"?std::uint64_t(0):reset;std::vector<std::uint64_t> words((port.type.width+63)/64,0);words[0]=r;
        f.options.input_reset_values[port.name]=words;
    }
    MutableProgram facts(f.p);facts.ensureValueFacts();f.p=facts.finish();f.placement.assign(f.p.signals.size(),0);
    for(const auto& port:f.p.ports)if(port.direction==PortDirection::Output)for(auto id:port.element_nodes)f.placement[id]=n;
    return f;
}
static Fixture patterned(int variable,int split,int zeros,bool known_one=false) {
    auto f=fixture(variable,1);
    f.p.ports.erase(std::remove_if(f.p.ports.begin(),f.p.ports.end(),[](const auto& p){return p.name=="y";}),f.p.ports.end());
    f.p.outputs.erase(std::remove(f.p.outputs.begin(),f.p.outputs.end(),"y"),f.p.outputs.end());
    Operand zero;zero.kind=OperandKind::Literal;zero.type.width=zeros;zero.constant.width=zeros;zero.constant.limbs={known_one?std::uint64_t(1):std::uint64_t(0)};
    NodeId value;
    if(split==variable)value=add(f.p,variable+zeros,OperationKind::Concat,{zero,ref(f.p,f.data)});
    else {
        auto low=add(f.p,split,OperationKind::Slice,{ref(f.p,f.data)});f.p.signal(low).driver->lo=0;f.p.signal(low).driver->hi=split-1;
        auto high=add(f.p,variable-split,OperationKind::Slice,{ref(f.p,f.data)});f.p.signal(high).driver->lo=split;f.p.signal(high).driver->hi=variable-1;
        value=add(f.p,variable+zeros,OperationKind::Concat,{ref(f.p,high),zero,ref(f.p,low)});
    }
    output(f.p,value,"y");MutableProgram facts(f.p);facts.ensureValueFacts();f.p=facts.finish();
    f.placement.assign(f.p.signals.size(),0);for(const auto& p:f.p.ports)if(p.direction==PortDirection::Output)for(auto id:p.element_nodes)f.placement[id]=1;
    return f;
}
static O priceJson(const StateCostEstimate& price) {return O{{"area_um2",price.area_um2},{"input_budget_ps",price.input_budget_ps},
    {"register_budget_ps",price.register_budget_ps},{"bits",price.bits},{"signals",price.signals},
    {"model_sha256",price.model_sha256},{"layout_sha256",price.layout_sha256},{"joint_arithmetic_boundary_qualified",price.joint_arithmetic_boundary_qualified}};}
int main(int argc,char** argv){try {
    check(argc==4||argc==5,"root, component manifest, output, optional public cases required");const std::filesystem::path root=argv[1],manifest=argv[2],out=argv[3];std::filesystem::create_directories(out);
    rtlzz::ArithmeticRequest r;r.target_platform="asap7";r.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();r.platform_profile_sha256=rtlzz::arithmeticHash(read(r.platform_profile));r.cost_mode="backend-model";r.model_bundle=manifest.string();r.model_bundle_sha256=rtlzz::arithmeticHash(read(manifest));r.latency=2;r.target_period_ps=1500;r.candidate_policy="mul-candidates-v1";
    rtlzz::validateArithmeticRequest(r,1);
    const auto state_path=root/"experiments/arithmetic/evidence/step7_native_state_v1/asap7-native-state.json";
    const auto state_hash=rtlzz::arithmeticHash(read(state_path));
    llvm::json::Array rows,rejections;
    auto refused=[&](const std::string& label,auto action){bool failed=false;try{action();}catch(const rtlzz::ArithmeticError& e){failed=e.diagnostic.failure_code=="cost_uncovered";rejections.push_back(O{{"test",label},{"reason",e.diagnostic.failure_reason}});}check(failed,label+" must reject as cost_uncovered");};
    auto record=[&](const std::string& name,const Program& p,const StateCostQuery& q,const StateCostEstimate& price,bool materialized){
        auto parsed=llvm::json::parse(stateCostQueryJson(q));check(bool(parsed),"query JSON");rows.push_back(O{{"case",name},{"query",std::move(*parsed)},{"price",priceJson(price)},
            {"state_component_price_bound",true},{"materialized_price_checked",materialized}});
    };
    // Real native materialization, all qualified widths, exact targets, two
    // register depths, both core-only and explicitly owned output captures.
    for(int width:{1,2,4,5,8,16,17,24,32,33,37,48,64,65})for(int n:{1,2}) {
        auto f=fixture(width,n);auto actual=pipeline::build(f.p,f.options);
        for(double t:{375.,1500.,40500.})for(bool external:{false,true}) {
            auto request=r;request.target_period_ps=t;request.latency=n+1;
            auto provider=makeCostProvider(actual.combinational,request);check(provider->requiresStateQuery(),"typed factory route");
            auto q=stateCostQuery(actual,t,external);auto price=provider->stateCost(actual.combinational,q);
            NativeStateModel standalone(state_path.string(),state_hash,request);auto bound=standalone.price(actual,external);
            check(price.area_um2==bound.area_um2&&price.layout_sha256==bound.layout_sha256,"composite/actual native binding mismatch");
            record("native-w"+std::to_string(width)+"-N"+std::to_string(n),actual.combinational,q,price,true);
        }
    }
    auto f=fixture(8,1,0,true);auto provider=makeCostProvider(f.p,r);auto before=stateCostQuery(f.p,f.options,f.placement,1500,false);auto first=provider->stateCost(f.p,before);
    auto moved=f.placement;moved[f.computation]=1;auto after=stateCostQuery(f.p,f.options,moved,1500,false);auto second=provider->stateCost(f.p,after);
    check(first.area_um2!=second.area_um2&&before.layout_sha256!=after.layout_sha256,"trial move must refresh actual bank price");
    record("before-move",f.p,before,first,false);record("after-move",f.p,after,second,false);
    auto stale=before;stale.placement=moved;refused("stale-placement",[&]{provider->stateCost(f.p,stale);});
    stale=before;stale.features["state_bits"]+=1;refused("forged-feature",[&]{provider->stateCost(f.p,stale);});
    stale=before;stale.slots.front().variable_mask.front()^=1;refused("forged-mask",[&]{provider->stateCost(f.p,stale);});
    stale=before;stale.slots.front().reset_values.front()^=1;refused("forged-reset",[&]{provider->stateCost(f.p,stale);});
    stale=before;stale.slots.front().enable_stage=9;refused("forged-enable",[&]{provider->stateCost(f.p,stale);});
    auto other=fixture(16,1);refused("different-graph",[&]{provider->stateCost(other.p,before);});
    refused("scalar-state-bypass",[&]{provider->stateArea(17,3,8,8);});
    refused("scalar-boundary-bypass",[&]{provider->dataBudget(true);});
    refused("joint-boundary-unqualified",[&]{provider->validateInterface(f.p,"ov");});
    refused("missing-stage-price",[&]{provider->stageDelay(1,1,8);});
    // Related channels across a cut are not independent bank inputs.
    auto correlated=stateCostQuery(f.p,f.options,moved,1500,true);
    refused("dependent-channels",[&]{provider->stateCost(f.p,correlated);});
    auto large=fixture(128,1);refused("wide-word",[&]{provider->stateCost(large.p,stateCostQuery(large.p,large.options,large.placement,1500,true));});
    auto deep=fixture(8,3);refused("four-capture-stages",[&]{provider->stateCost(deep.p,stateCostQuery(deep.p,deep.options,deep.placement,1500,true));});
    auto wrong=f.options;wrong.reset_active_low=false;refused("reset-polarity",[&]{stateCostQuery(f.p,wrong,f.placement,1500,true);});
    auto resets=fixture(8,1,0x53);refused("unregistered-reset-pattern",[&]{provider->stateCost(resets.p,stateCostQuery(resets.p,resets.options,resets.placement,1500,true));});
    auto nonzero_valid=fixture(8,1);nonzero_valid.options.input_reset_values["iv"]={1};
    refused("nonzero-valid-reset",[&]{provider->stateCost(nonzero_valid.p,stateCostQuery(nonzero_valid.p,nonzero_valid.options,nonzero_valid.placement,1500,true));});
    // Preserve legal nonzero reset contexts and exact input-delay ownership.
    for(auto reset:{std::uint64_t(0xff),std::uint64_t(0x55),std::uint64_t(0x49)}) {
        auto good=fixture(8,1,reset);auto q=stateCostQuery(good.p,good.options,good.placement,1500,true);record("registered-reset",good.p,q,provider->stateCost(good.p,q),false);
    }
    for(auto good:{patterned(4,3,1),patterned(64,64,1)}) {
        auto q=stateCostQuery(good.p,good.options,good.placement,1500,true);record("known-zero-hole-or-padding",good.p,q,provider->stateCost(good.p,q),false);
    }
    auto ones=patterned(4,3,1,true);refused("known-one-data",[&]{provider->stateCost(ones.p,stateCostQuery(ones.p,ones.options,ones.placement,1500,true));});
    auto holes=patterned(6,4,2);refused("multiple-internal-holes",[&]{provider->stateCost(holes.p,stateCostQuery(holes.p,holes.options,holes.placement,1500,true));});
    auto overlap=fixture(8,1);auto duplicate=overlap.p.ports[overlap.p.ports.size()-2];duplicate.name="duplicate_y";overlap.p.ports.push_back(duplicate);overlap.p.outputs.push_back(duplicate.name);
    refused("aliased-double-capture",[&]{provider->stateCost(overlap.p,stateCostQuery(overlap.p,overlap.options,overlap.placement,1500,true));});
    auto control_data=fixture(1,1);NodeId control=kInvalidNodeId;for(const auto& port:control_data.p.ports)if(port.name=="iv")control=port.element_nodes.front();
    output(control_data.p,control,"control_copy");control_data.placement.push_back(1);
    refused("data-dependent-on-valid",[&]{provider->stateCost(control_data.p,stateCostQuery(control_data.p,control_data.options,control_data.placement,1500,true));});
    auto materialized=pipeline::build(other.p,other.options);auto changed=materialized;
    changed.registers.front().enable=changed.valid_nodes.back();NativeStateModel native_model(state_path.string(),state_hash,r);
    bool materialized_rejected=false;try{native_model.price(changed,true);}catch(const std::exception&){materialized_rejected=true;}
    check(materialized_rejected,"actual wrong-enable register must reject before pricing");
    rejections.push_back(O{{"test","actual-materialized-wrong-enable"},{"reason","actual native register protocol rejected before price"}});
    auto bad=r;bad.target_period_ps=750;refused("nonqualified-exact-clock",[&]{makeCostProvider(f.p,bad);});
    bad=r;bad.target_period_ps=std::numeric_limits<double>::quiet_NaN();refused("nonfinite-clock",[&]{makeCostProvider(f.p,bad);});
    bad=r;bad.model_bundle_sha256="bad";refused("manifest-hash",[&]{makeCostProvider(f.p,bad);});
    bad=r;bad.platform_profile_sha256="bad";refused("profile-identity",[&]{makeCostProvider(f.p,bad);});
    // Mutated manifests are rehashed, so these check semantic guards rather
    // than stopping at the outer hash. Frozen component bytes stay intact.
    int mutation=0;
    auto mutate=[&](const std::string& label,auto change){
        auto parsed=llvm::json::parse(read(manifest));auto& b=*parsed->getAsObject();
        for(auto& item:*b.getObject("components")){auto& c=*item.second.getAsObject();c["path"]=(manifest.parent_path()/c.getString("path")->str()).lexically_normal().string();}
        change(b);auto path=out/("bad-manifest-"+std::to_string(++mutation)+".json");std::ofstream(path)<<llvm::formatv("{0:2}",*parsed).str();auto req=r;req.model_bundle=path.string();req.model_bundle_sha256=rtlzz::arithmeticHash(read(path));refused(label,[&]{makeCostProvider(f.p,req);});
    };
    mutate("wrong-stage",[](O& b){b["evaluation_stage"]="synth";});
    mutate("component-hash",[](O& b){(*b.getObject("components")->getObject("native_state"))["sha256"]="bad";});
    mutate("component-schema",[](O& b){(*b.getObject("components")->getObject("native_state"))["schema"]="unknown";});
    mutate("false-full-D-claim",[](O& b){b["full_D_ready"]=true;});
    mutate("false-joint-boundary-claim",[](O& b){b["joint_arithmetic_boundary_qualified"]=true;});
    auto mutate_state=[&](const std::string& label,auto change){
        auto parsed=llvm::json::parse(read(state_path));change(*parsed->getAsObject());auto path=out/("bad-state-"+std::to_string(++mutation)+".json");std::ofstream(path)<<llvm::formatv("{0:2}",*parsed).str();
        mutate(label,[&](O& b){auto& c=*b.getObject("components")->getObject("native_state");c["path"]=path.string();c["sha256"]=rtlzz::arithmeticHash(read(path));});
    };
    mutate_state("native-state-unknown-schema",[](O& b){b["version"]="unknown";});
    mutate_state("native-state-failed-quality",[](O& b){b["quality_status"]="failed";});
    mutate_state("native-state-parent-identity",[](O& b){b["parent_primitives_sha256"]="wrong";});
    mutate_state("native-state-physical-identity",[](O& b){b["measurement_static_identity_sha256"]="wrong";});
    mutate_state("native-state-domain-extension",[](O& b){(*b.getObject("domain"))["max_capture_stage"]=4;});
    // Actual public production selection uses the new manifest and retains
    // all three attempts with the more precise remaining qualification gap.
    pipelinev2::PipelineConfig c;c.source_name="component-D.cpp";c.source_text="#include <fixint.hpp>\nInt<8>a,b;Int<16>y;bool iv,ov;\n#pragma input_port a\n#pragma input_port b\n#pragma input_port iv\n#pragma output_port y\n#pragma output_port ov\nvoid top(){y=a*b;}\n";c.top_function="top";c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;c.arithmetic_request=r;c.pipeline.emplace();c.pipeline->register_stages=1;c.pipeline->reset_enabled=true;c.pipeline->input_valid_name="iv";c.pipeline->output_valid_name="ov";c.pipeline->input_reset_values={{"a",{0}},{"b",{0}},{"iv",{0}}};
    auto result=pipelinev2::compile(c);check(!result.ok()&&result.arithmetic_report.find("component_manifest_missing_joint_arithmetic_and_service_boundary_qualification")!=std::string::npos,"production D must retain remaining joint qualification refusal");
    auto diagnostic=llvm::json::parse(result.arithmetic_report);check(bool(diagnostic),"D report JSON");check(diagnostic->getAsObject()->getObject("selection")->getArray("candidates")->size()==3,"ordered three D candidates");std::ofstream(out/"D-diagnostic.json")<<result.arithmetic_report;
    if(argc==5) {
        auto cases=llvm::json::parse(read(argv[4]));check(bool(cases),"public native case manifest");llvm::json::Array banks;
        for(const auto& item:*cases->getAsObject()->getArray("cases")) {
            const auto& entry=*item.getAsObject();auto name=entry.getString("name")->str();
            pipelinev2::PipelineConfig config;config.source_name=entry.getString("source")->str();config.top_function=entry.getString("top")->str();config.clang_args=c.clang_args;config.use_circt=false;config.pipeline.emplace();auto& options=*config.pipeline;
            options.register_stages=*entry.getInteger("N");options.reset_enabled=true;options.input_valid_name=entry.getString("iv")->str();options.output_valid_name=entry.getString("ov")->str();
            for(const auto& reset:*entry.getObject("reset")){std::vector<std::uint64_t> words;for(const auto& w:*reset.second.getAsArray())words.push_back(std::stoull(w.getAsString()->str(),nullptr,16));options.input_reset_values[reset.first.str()]=words;}
            auto fixed=r;fixed.latency=options.register_stages+1;fixed.cost_mode="fixed";fixed.model_bundle.clear();fixed.model_bundle_sha256.clear();fixed.candidate_policy="core-original-v1";fixed.fixed_cost_table=(root/"experiments/arithmetic_dse/configs/fixed_op_width_v1.json").string();fixed.fixed_cost_table_sha256=rtlzz::arithmeticHash(read(fixed.fixed_cost_table));config.arithmetic_request=fixed;
            auto compiled=pipelinev2::compile(config);check(compiled.ok()&&compiled.pipeline_program.has_value(),name+": "+compiled.error);const auto& actual=*compiled.pipeline_program;
            auto q=stateCostQuery(actual,1500,true);auto parsed=llvm::json::parse(stateCostQueryJson(q));O row{{"case",name},{"query",std::move(*parsed)},
                {"source_sha256",rtlzz::arithmeticHash(read(config.source_name))},{"original_core_only",true},{"all_D_candidates_or_moves_checked",false}};
            try{auto cost=native_model.price(actual,true);row["status"]="state_bank_predicted_in_domain";row["price"]=priceJson(cost);}
            catch(const rtlzz::ArithmeticError& e){row["status"]="cost_uncovered";row["reason"]=e.diagnostic.failure_reason;}
            banks.push_back(std::move(row));
        }
        std::ofstream(out/"public-bank-coverage.json")<<llvm::formatv("{0:2}",V(O{{"version","native-state-production-public-coverage-v1"},{"rows",std::move(banks)},
            {"PPA_calls",0},{"full_D_ready",false},{"formal_target_qualification",false}})).str();
    }
    std::ofstream(out/"queries.json")<<llvm::formatv("{0:2}",V(O{{"version","native-state-production-binding-v1"},{"rows",std::move(rows)},
        {"rejections",std::move(rejections)},{"PPA_calls",0},{"full_D_ready",false}})).str();
    std::cout<<"PASS composite production factory, native state price, trial refresh and qualified-domain refusals\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
