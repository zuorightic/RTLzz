#include "backend/arithmetic_selection.hpp"
#include "backend/arithmetic_core.hpp"
#include "backend/mul_split.hpp"
#include "backend/rtlgen.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <functional>
#include <algorithm>
#include <cmath>
#include <set>
#include <tuple>
namespace pred::beir::arithmetic {
Selection select(const Program& source,const pipeline::Options& options){
    using namespace llvm::json;
    const auto& request=*options.arithmetic_request;
    std::vector<const ArithmeticRegion*> regions;for(const auto& r:source.arithmetic_regions)if(r.template_version=="mul-original-v1")regions.push_back(&r);
    if(regions.empty()&&request.candidate_policy=="mul-template-v1")throw rtlzz::ArithmeticError({"eligibility","template_ineligible","no eligible original Mul occurrence",{},{}});
    // The registered occurrences retain the original optimized Mul NodeId order.
    std::sort(regions.begin(),regions.end(),[](auto a,auto b){return a->members[0]<b->members[0];});
    Array attempts;std::set<std::string> shapes;std::size_t count=0,eligible=0;std::string chosen;
    std::optional<pipeline::Program> best;
    using Score=std::tuple<double,std::uint64_t,std::uint64_t,std::size_t,std::string>;
    std::optional<Score> bestScore;
    std::optional<rtlzz::ArithmeticDiagnostic> last_rejection;
    std::vector<mul_split::Configuration> configs;std::vector<std::string> labels;
    auto evaluate=[&](){
        if(count>=16)return;std::string id="candidate";for(const auto& x:labels)id+="/"+x;
        Program graph;
        try{graph=mul_split::expand(source,configs);}
        catch(const rtlzz::ArithmeticError& e){last_rejection=e.diagnostic;attempts.push_back(Object{{"id",id},{"status","ineligible"},{"failure_stage",e.diagnostic.failure_stage},{"failure_code",e.diagnostic.failure_code},{"reason",e.diagnostic.failure_reason}});return;}
        ++eligible;
        // BEIR text includes precise graph, region and parameter identities.
        auto hash=rtlzz::arithmeticHash(emitText(graph));if(!shapes.insert(hash).second)return;
        ++count;
        try{
            auto p=pipeline::build(graph,options);pipeline::validateProgram(p);validate(p.rtl_program,"candidate_materialization",false,{options.clock_name,options.reset_name});
            const auto& r=p.report;if(r.combinational_cost>UINT64_MAX-r.register_bits)throw rtlzz::ArithmeticError({"cost","cost_uncovered","combined fixed score overflow",{},{}});Score score{r.final_delay,r.combinational_cost+r.register_bits,r.register_bits,r.register_count,id};
            Array initial,final,moves;for(int s:p.initial_stages)initial.push_back(s);for(int s:p.stages)final.push_back(s);
            for(const auto& m:p.moves){Object x{{"group",m.group},{"from",m.from},{"to",m.to},{"accepted",m.accepted},{"reason",m.reason},{"register_bits",m.bits}};if(std::isfinite(m.delay))x["delay"]=m.delay;else x["delay"]=nullptr;moves.push_back(std::move(x));}
            Array scoreJson{r.final_delay,r.combinational_cost+r.register_bits,r.register_bits,static_cast<std::uint64_t>(r.register_count),id};
            auto computation=llvm::json::parse(arithmetic::report(p.combinational,request,&p.report,p.stages,"",&options));
            attempts.push_back(Object{{"computation",std::move(*computation)},{"id",id},{"status","scheduled"},{"shape_sha256",hash},{"maximum_delay",r.final_delay},{"comb",r.combinational_cost},{"register_bits",r.register_bits},{"register_count",static_cast<std::uint64_t>(r.register_count)},{"score",std::move(scoreJson)},{"initial_stages",std::move(initial)},{"final_stages",std::move(final)},{"moves",std::move(moves)}});
            if(!bestScore||score<*bestScore){bestScore=score;best=std::move(p);chosen=id;}
        }catch(const rtlzz::ArithmeticError& e){last_rejection=e.diagnostic;attempts.push_back(Object{{"id",id},{"status","rejected"},{"failure_code",e.diagnostic.failure_code},{"failure_stage",e.diagnostic.failure_stage},{"reason",e.diagnostic.failure_reason}});}
    };
    std::function<void(std::size_t)> enumerate=[&](std::size_t i){
        if(count>=16)return;
        if(i==regions.size()){evaluate();return;}
        const auto& r=*regions[i];const int w=std::max(r.template_parameters.at("lhs_width"),r.template_parameters.at("rhs_width"));
        if(request.candidate_policy=="mul-template-v1"){
            int b=request.template_parameters.at("block_width");configs.push_back({r.id,b});labels.push_back("block"+std::to_string(b));enumerate(i+1);configs.pop_back();labels.pop_back();return;
        }
        for(const auto& descriptor:mul_split::registry()){
            std::string mode=descriptor.id;
            if(request.candidate_policy=="core-original-v1" && std::string(mode)!="original")continue;
            int b=descriptor.block_divisor?std::max(2,w/descriptor.block_divisor):0;
            if(b && (b>w || r.template_parameters.at("result_width")>r.template_parameters.at("lhs_width")+r.template_parameters.at("rhs_width")))continue;
            if(std::string(mode)=="quarter" && b==std::max(2,w/2))continue;
            configs.push_back({r.id,b});labels.push_back(mode);enumerate(i+1);configs.pop_back();labels.pop_back();
        }
    };
    enumerate(0);
    if(!best) {
        if(last_rejection)throw rtlzz::ArithmeticError(*last_rejection);
        throw rtlzz::ArithmeticError({"scheduling","schedule_no_placement_found","no scheduled implementation in declared candidate set",{}, {}});
    }
    Object report{{"version","arithmetic-selection-v1"},{"policy",request.candidate_policy},{"K",16},{"eligible_configurations",static_cast<std::uint64_t>(eligible)},{"evaluated_structures",static_cast<std::uint64_t>(count)},{"enumeration_limit_reached",count>=16},{"selected_candidate",chosen},{"formal_C_selection",request.candidate_policy=="mul-candidates-v1"},{"diagnostic_forced_template",request.candidate_policy=="mul-template-v1"},{"candidate_PPA_calls",0},{"candidates",std::move(attempts)}};
    return {std::move(*best),chosen,llvm::formatv("{0:2}",Value(std::move(report))).str()};
}
}
