#include "backend/arithmetic_bounded_search.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <tuple>

namespace pred::beir::arithmetic {
namespace {
bool wiring(const Operation& d){
    switch(d.kind){case OperationKind::Assign:case OperationKind::Cast:case OperationKind::ZExt:
    case OperationKind::Trunc:case OperationKind::Slice:case OperationKind::BitSelect:
    case OperationKind::Concat:case OperationKind::PortRead:return true;default:return false;}
}
struct Partial {std::vector<int> stages,loads;std::size_t score=0;};
}
BoundedSchedules boundedSchedules(const Program& p,const pipeline::Options& options,const BoundedSchedulePolicy& policy){
    if(!policy.beam_width||policy.beam_width>4096||!policy.max_extensions||policy.max_extensions>10000000
       ||options.register_stages<0||options.register_stages>255)
        throw std::runtime_error("invalid finite schedule budget or stage count");
    if(!options.arithmetic_request||options.arithmetic_request->cost_mode!="fixed")
        throw std::runtime_error("bounded schedules need explicit fixed structural context");
    const auto units=pipeline::diagnosticSchedulingUnits(p,options);
    std::vector<std::size_t> owners(p.signals.size(),std::numeric_limits<std::size_t>::max());
    std::vector<bool> outputs(p.signals.size());
    for(const auto& port:p.ports)if(port.direction==PortDirection::Output)for(auto n:port.element_nodes)outputs[n]=true;
    for(std::size_t i=0;i<units.size();++i)for(auto n:units[i].members)owners[n]=i;
    std::vector<Partial> frontier{{std::vector<int>(p.signals.size()),std::vector<int>(options.register_stages+1)}};
    BoundedSchedules out;
    for(std::size_t i=0;i<units.size();++i){
        const auto& unit=units[i];std::vector<Partial> next;
        for(const auto& prior:frontier){
            int lower=0;bool output=false,pure=true;
            for(auto n:unit.members){output|=outputs[n];pure&=wiring(*p.signal(n).driver);
                for(const auto& a:p.signal(n).driver->operands)if(a.kind==OperandKind::Symbol&&owners[a.node]!=i){
                    // Use the scheduler's structural constant classification.
                    // A value proved constant can still have live dependency
                    // edges until cleanup; its producer stage remains binding.
                    if(owners[a.node]<units.size()&&units[owners[a.node]].constant)continue;
                    if(owners[a.node]>=i)throw std::runtime_error("non-topological scheduling units");
                    lower=std::max(lower,prior.stages[a.node]);
                }
            }
            int first=lower,last=options.register_stages;
            if(unit.fixed)first=last=output?last:0;
            else if(unit.constant)first=last=0;
            else if(pure)last=first;
            for(int at=first;at<=last;++at){
                if(at<lower&&!unit.constant)continue;
                if(out.extensions==policy.max_extensions){out.budget_exhausted=true;out.exhaustive=false;return out;}
                ++out.extensions;auto trial=prior;
                for(auto n:unit.members)trial.stages[n]=at;
                if(!pure&&!unit.constant){auto old=trial.loads[at]++;trial.score+=2*std::size_t(old)+1;}
                next.push_back(std::move(trial));
            }
        }
        std::sort(next.begin(),next.end(),[](const auto& a,const auto& b){return std::tie(a.score,a.stages)<std::tie(b.score,b.stages);});
        if(next.size()>policy.beam_width){out.beam_discarded+=next.size()-policy.beam_width;next.resize(policy.beam_width);out.exhaustive=false;}
        frontier=std::move(next);if(frontier.empty())break;
    }
    for(auto& row:frontier){
        if(std::isfinite(pipeline::evaluateSchedule(p,options,row.stages).final_delay))out.placements.push_back(std::move(row.stages));
        else ++out.invalid_leaves;
    }
    return out;
}
llvm::json::Object boundedScheduleReport(const BoundedSchedules& r,const BoundedSchedulePolicy& p){
    return llvm::json::Object{{"version","bounded-topological-schedules-v1"},{"beam_width",std::uint64_t(p.beam_width)},
        {"max_extensions",std::uint64_t(p.max_extensions)},{"explored_extensions",std::uint64_t(r.extensions)},
        {"beam_discarded",std::uint64_t(r.beam_discarded)},{"invalid_leaves",std::uint64_t(r.invalid_leaves)},
        {"returned_placements",std::uint64_t(r.placements.size())},{"exhaustive_within_wiring_grammar",r.exhaustive},
        {"budget_exhausted",r.budget_exhausted},{"wiring_grammar","earliest dependency stage; output ports fixed at last stage"},
        {"ranking","sum of squared arithmetic scheduling-unit counts per stage; stable stage-vector tie"},
        {"physical_price_pruning",false},{"global_optimum_claimed",false},{"price_qualification_granted",false}};
}
}
