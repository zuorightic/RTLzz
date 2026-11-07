// Reuse only synthetic BEIR fixture construction; no published physical label
// or model is replaced by this explicitly undeployable mechanism provider.
#include "arithmetic_general_state_fixture.hpp"
#include "backend/arithmetic_placement.hpp"
#include "backend/mul_split.hpp"
#include <set>
#include <tuple>

struct ContractMechanismOnly final : CostProvider {
    std::string mutation;
    static inline int complete_calls=0,partial_calls=0;
    explicit ContractMechanismOnly(std::string m={}):mutation(std::move(m)){context_.model_bundle_sha256=rtlzz::arithmeticHash("MECHANISM_ONLY_NOT_A_PHYSICAL_MODEL");}
    bool requiresCompletePlacementCost() const override{return true;}
    CostEstimate node(const Program&,NodeId) const override{throw std::runtime_error("complete stage must never ask for a substitute node price");}
    CompletePlacementPrice completePlacementCost(const Program&,const CompletePlacementCostQuery& q) const override{
        ++complete_calls;
        if(mutation=="all-uncovered")throw rtlzz::ArithmeticError({"cost","cost_uncovered","test-only missing domain",{},{}});
        CompletePlacementPrice p;p.query_sha256=q.query_sha256;p.model_sha256=rtlzz::arithmeticHash("MECHANISM_ONLY_NOT_A_PHYSICAL_MODEL");
        p.joint_source_and_boundary_qualified=true;
        for(const auto& cut:q.stages){StagePlacementPrice s;s.index=cut.index;s.cut_sha256=cut.cut_sha256;s.model_sha256=p.model_sha256;
            s.state_boundary_sha256=q.storage_boundaries[cut.index].sha256;
            s.ownership="whole_stage";s.owned_members=cut.members;s.QD_ps=1+cut.members.size();s.clock_capture_overhead_ps=20+cut.index;s.combinational_area_um2=2*cut.members.size();
            for(const auto& i:q.intervals)if(i.stage==cut.index)s.interval_qualification_sha256[i.query_sha256]=rtlzz::arithmeticHash("MECHANISM_I_ONLY");
            p.stages.push_back(std::move(s));}
        for(const auto& [owner,f]:q.state.owner_features){p.functional_state_area_um2[owner]=f.at("state_bits")*.5+f.at("state_signals");p.state_owner_model_sha256[owner]=p.model_sha256;}
        p.shared_physical_area_um2=7;p.shared_physical_model_sha256=p.model_sha256;
        for(const auto& name:{"native_enable","native_valid","external_data","external_valid","input_data","input_enable","input_valid"})p.control_boundary_upper_ps[name]=35;
        if(mutation=="missing-stage")p.stages.pop_back();
        if(mutation=="foreign-model")p.model_sha256=rtlzz::arithmeticHash("FOREIGN_MODEL");
        if(mutation=="duplicate-owner")p.stages.front().owned_members.push_back(p.stages.front().owned_members.front());
        if(mutation=="missing-state")p.functional_state_area_um2.erase("input_service");
        if(mutation=="shared-unqualified")p.shared_physical_model_sha256.clear();
        if(mutation=="joint-unqualified")p.joint_source_and_boundary_qualified=false;
        if(mutation=="missing-control")p.control_boundary_upper_ps.erase("input_valid");
        if(mutation=="foreign-cut")p.stages.front().cut_sha256="changed";
        if(mutation=="foreign-storage")p.stages.front().state_boundary_sha256="changed";
        if(mutation=="bad-area")p.stages.front().combinational_area_um2=-1;
        if(mutation=="nonfinite")p.shared_physical_area_um2=std::numeric_limits<double>::quiet_NaN();
        if(mutation=="missing-I")for(auto& s:p.stages)s.interval_qualification_sha256.clear();
        if(mutation=="tight-stage")p.stages.front().QD_ps=q.state.core.period_ps;
        return p;
    }
    PartialPlacementBound partialPlacementBound(const Program& p,const PartialPlacementCostQuery& q) const override{
        ++partial_calls;
        if(mutation=="unknown-zero"){PartialPlacementBound b;b.maximum_stage_delay_lower_ps=0;return b;}
        return CostProvider::partialPlacementBound(p,q);
    }
};
#ifdef RTLZZ_SCHEDULER_CONTRACT_TEST
// The dedicated regression compiles the real pipeline.cpp once with only its
// factory symbol renamed. This symbol and provider are absent from production.
static std::string scheduler_mutation;
namespace pred::beir::arithmetic {
std::unique_ptr<CostProvider> makeContractTestCostProvider(const Program&,const rtlzz::ArithmeticRequest&){
    return std::make_unique<ContractMechanismOnly>(scheduler_mutation);
}
}
#endif
static Fixture multiplication(int width,int divisor,bool low){
    Fixture f;f.p.function_name="IntervalPlacementContract";auto a=input(f.p,width,"a"),b=input(f.p,width,"b"),iv=input(f.p,1,"iv");
    auto mul=add(f.p,low?width:2*width,OperationKind::Binary,{ref(f.p,a),ref(f.p,b)},OpCode::Mul);f.p.signal(mul).driver->arithmetic_origin="contract-native-mul";
    output(f.p,mul,"y");output(f.p,iv,"ov");f.p=registerOriginalOperators(std::move(f.p));auto region=f.p.arithmetic_regions.front().id;
    f.p=mul_split::expand(std::move(f.p),{{region,width/divisor}});MutableProgram facts(f.p);facts.ensureValueFacts();f.p=facts.finish();
    f.options.register_stages=1;f.options.reset_enabled=true;f.options.input_valid_name="iv";f.options.output_valid_name="ov";f.options.input_reset_values={{"a",{0x8035}},{"b",{0x8035}},{"iv",{0}}};
    f.service.mode=1;f.service.valid_write_enable="we_data";f.service.data_write_enables={{"a","we_data"},{"b","we_data"}};f.service.external_data_unconditional=f.service.external_valid_unconditional=true;
    return f;
}
int main(int argc,char** argv){try{
    check(argc==2,"out required");std::filesystem::path out=argv[1];std::filesystem::create_directories(out);
    ContractMechanismOnly provider;auto f=fixture(2,1,3);
    auto before=completePlacementCostQuery(f.p,f.options,f.stages,1119,f.service);auto first=evaluateCompletePlacement(f.p,before,provider);
    auto moved=f.stages;moved[f.data.front()]=1;auto after=completePlacementCostQuery(f.p,f.options,moved,1119,f.service);auto second=evaluateCompletePlacement(f.p,after,provider);
    check(before.query_sha256!=after.query_sha256&&first.total_area_um2!=second.total_area_um2,"trial must rebuild full ownership and prices");
    auto actual=pipeline::build(f.p,f.options);auto final=evaluateCompletePlacement(actual,1119,f.service,provider);
    auto projected=completePlacementCostQuery(actual.combinational,actual.options,actual.stages,1119,f.service);auto final_projected=evaluateCompletePlacement(actual.combinational,projected,provider);
    check(final.price.query_sha256==final_projected.price.query_sha256&&final.total_area_um2==final_projected.total_area_um2,"actual final query must match projected query");
    check(first.stage_budgets_ps[0]==1099&&first.boundary_margins_ps.at("input_data")==1119*.8-35,
        "registered service core stage must not subtract host IO twice");
    check(first.total_area_um2==first.combinational_area_um2+first.functional_state_area_um2+7,"shared physical owner exactly once");
    A rejections;
    auto refused=[&](const std::string& name,auto action){bool denied=false;try{action();}catch(const rtlzz::ArithmeticError& e){denied=e.diagnostic.failure_code=="cost_uncovered";}
        check(denied,"missing complete-price rejection: "+name);rejections.push_back(name);};
    for(const auto& name:{"missing-stage","foreign-model","duplicate-owner","missing-state","shared-unqualified","joint-unqualified","missing-control","foreign-cut","foreign-storage","bad-area","nonfinite"})
        refused(name,[&]{ContractMechanismOnly bad(name);evaluateCompletePlacement(f.p,before,bad);});
    auto changed=before;changed.state.owner_features["native_state"]["data_bits"]+=1;
    refused("forged-state-in-complete-query",[&]{evaluateCompletePlacement(f.p,changed,provider);});
    changed=before;changed.stages[0].captures.push_back({0,99});refused("forged-capture",[&]{evaluateCompletePlacement(f.p,changed,provider);});
    changed=before;changed.state.core.slots.clear();refused("forged-core-slots",[&]{evaluateCompletePlacement(f.p,changed,provider);});
    auto partial=f.stages;for(auto id:f.data)partial[id]=-1;for(const auto& port:f.p.ports)if(port.direction==PortDirection::Output)for(auto id:port.element_nodes)partial[id]=-1;
    auto query=partialPlacementCostQuery(f.p,f.options,partial,1119,f.service);auto unknown=evaluatePartialPlacement(f.p,query,provider);
    check(!unknown.certified&&!unknown.maximum_stage_delay_lower_ps&&!unknown.total_area_lower_um2,"unknown partial price is not zero");
    refused("unknown-partial-zero-bypass",[&]{ContractMechanismOnly bad("unknown-zero");evaluatePartialPlacement(f.p,query,bad);});
    ContractMechanismOnly tight("tight-stage");check(!evaluateCompletePlacement(f.p,before,tight).feasible,"covered negative slack is infeasible, not uncovered");
    A intervals;std::set<std::string> seen;int complete_queries=0;
    for(auto shape:std::vector<std::tuple<int,int,bool>>{{16,2,false},{16,4,false},{32,2,false},{32,4,false},{32,2,true},{32,4,true}}){
        auto [width,divisor,low]=shape;auto g=multiplication(width,divisor,low);const auto& region=g.p.arithmetic_regions.front();int count=region.legal_cut_groups.size();
        for(int boundary=0;boundary<=count;++boundary){std::vector<int> placement(g.p.signals.size(),0);
            std::set<NodeId> fixed_group_members;
            for(int k=boundary;k<count;++k)for(auto id:region.legal_cut_groups[k])placement[id]=1;
            for(const auto& group:region.legal_cut_groups)fixed_group_members.insert(group.begin(),group.end());
            for(const auto& port:g.p.ports)if(port.direction==PortDirection::Output)for(auto id:port.element_nodes)placement[id]=1;
            // Actual template expansion contains wiring outside legal groups.
            // Place those aliases after their producers; they are not free to
            // reverse a dependency merely because a group map was supplied.
            bool updated=true;for(std::size_t pass=0;updated&&pass<g.p.signals.size();++pass){updated=false;
                for(const auto& s:g.p.signals)if(!fixed_group_members.count(s.id)&&s.driver){
                    int required=placement[s.id];for(const auto& op:s.driver->operands)if(op.kind==OperandKind::Symbol)required=std::max(required,placement[op.node]);
                    if(required!=placement[s.id]){placement[s.id]=required;updated=true;}}}
            auto q=completePlacementCostQuery(g.p,g.options,placement,1431,g.service);auto value=evaluateCompletePlacement(g.p,q,provider);++complete_queries;
            check(value.feasible,"contract mechanism interval price");
            for(const auto& i:q.intervals){auto key=std::to_string(width)+"/"+(low?"low":"full")+"/"+std::to_string(divisor)+"/"+std::to_string(i.first)+"/"+std::to_string(i.last);
                if(!seen.insert(key).second)continue;A inputs;std::uint64_t bits=0;for(auto node:i.inputs)inputs.push_back(i.input_bit_positions.at(node).size());for(const auto& [node,positions]:i.output_bit_positions)bits+=positions.size();
                intervals.push_back(O{{"width",width},{"divisor",divisor},{"low",low},{"first",i.first},{"last",i.last},{"input_widths",std::move(inputs)},{"capture_bits",bits}});
            }
            if(boundary==0){refused("missing-real-I-qualification",[&]{ContractMechanismOnly bad("missing-I");evaluateCompletePlacement(g.p,q,bad);});}
        }
    }
    check(seen.size()==42,"all42 current monotone interval shapes represented");
#ifdef RTLZZ_SCHEDULER_CONTRACT_TEST
    auto scheduled_fixture=fixture(2,1,3);auto options=scheduled_fixture.options;options.operation_delay_weights.clear();
    auto root=std::filesystem::path(__FILE__).parent_path().parent_path().parent_path();
    rtlzz::ArithmeticRequest request;request.cost_mode="backend-model";request.target_period_ps=1119;request.target_platform="asap7";request.latency=3;
    request.platform_profile=(root/"experiments/arithmetic_dse/configs/platforms/asap7.json").string();request.platform_profile_sha256=rtlzz::arithmeticHash(read(request.platform_profile));
    request.model_bundle=(root/"experiments/arithmetic_dse/configs/model_asap7_d_general_components_v1.json").string();request.model_bundle_sha256=rtlzz::arithmeticHash(read(request.model_bundle));
    request.candidate_policy="mul-candidates-v1";options.arithmetic_request=request;
    options.service_state_protocol=std::make_shared<ServiceStateProtocol>(scheduled_fixture.service);
    options.cost_search_deadline=std::chrono::steady_clock::now()+std::chrono::seconds(30);
    auto complete_before=ContractMechanismOnly::complete_calls,partial_before=ContractMechanismOnly::partial_calls;
    auto scheduled=pipeline::build(scheduled_fixture.p,options);
    check(scheduled.report.model_feasible&&!scheduled.moves.empty(),"actual complete scheduler initial/move/final branch");
    auto scheduled_final=evaluateCompletePlacement(scheduled,1119,scheduled_fixture.service,provider);
    check(scheduled_final.total_area_um2==scheduled.report.predicted_total_area_um2,"materialized scheduler price mismatch");
    check(ContractMechanismOnly::complete_calls>complete_before+2&&ContractMechanismOnly::partial_calls>partial_before,"actual DFS used complete/partial queries");
    auto expired=options;expired.cost_search_deadline=std::chrono::steady_clock::now()-std::chrono::seconds(1);bool deadline_refused=false;
    try{pipeline::build(scheduled_fixture.p,expired);}catch(const rtlzz::ArithmeticError& e){deadline_refused=e.diagnostic.failure_code=="schedule_no_placement_found";}
    check(deadline_refused,"expired shared search deadline must refuse without a global impossibility claim");
    scheduler_mutation="all-uncovered";bool search_limit_refused=false;
    try{pipeline::build(scheduled_fixture.p,options);}catch(const rtlzz::ArithmeticError& e){search_limit_refused=e.diagnostic.failure_code=="schedule_no_placement_found";}
    check(search_limit_refused,"4096-node exhaustion must preserve unknown search outcome");
    Program small;small.function_name="MissingCompleteDomain";auto sa=input(small,16,"a"),sb=input(small,16,"b"),sv=input(small,1,"iv");
    auto sum=add(small,16,OperationKind::Binary,{ref(small,sa),ref(small,sb)},OpCode::Add);output(small,sum,"y");output(small,sv,"ov");
    auto small_options=options;small_options.input_reset_values={{"a",{7}},{"b",{9}},{"iv",{0}}};bool missing_refused=false;
    try{pipeline::build(small,small_options);}catch(const rtlzz::ArithmeticError& e){missing_refused=e.diagnostic.failure_code=="cost_uncovered";}
    check(missing_refused,"bounded exhausted traversal must preserve missing-domain refusal");scheduler_mutation.clear();
    write(out/"scheduler.json",O{{"status","passed"},{"actual_pipeline_scheduler_branch",true},{"test_only_renamed_factory",true},
        {"complete_queries",ContractMechanismOnly::complete_calls-complete_before},{"partial_queries",ContractMechanismOnly::partial_calls-partial_before},
        {"trial_moves",scheduled.moves.size()},{"deadline_refused",deadline_refused},{"missing_domain_refused",missing_refused},{"search_limit_refused",search_limit_refused},
        {"materialized_final_area_parity",true},{"PPA_calls",0},{"physical_model_qualified",false},{"full_D_ready",false}});
#endif
    write(out/"mechanism.json",O{{"version","complete-placement-interface-mechanism-v1"},{"mechanism_only",true},{"physical_model_qualified",false},
        {"initial_query",parsed(completePlacementCostQueryJson(before))},{"trial_query",parsed(completePlacementCostQueryJson(after))},
        {"final_query",parsed(completePlacementCostQueryJson(completePlacementCostQuery(actual,1119,f.service)))},
        {"initial",parsed(completePlacementEvaluationJson(first))},{"trial",parsed(completePlacementEvaluationJson(second))},
        {"final",parsed(completePlacementEvaluationJson(final))},{"partial_unknown_is_zero",false},
        {"interval_query_count",complete_queries},{"distinct_intervals",42},{"intervals",std::move(intervals)},{"rejections",std::move(rejections)},
        {"PPA_calls",0},{"full_D_ready",false}});
    std::cout<<"PASS complete placement ownership/budgets/partial safety and42 real typed interval shapes; mechanism only\n";
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
