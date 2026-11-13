#include "backend/arithmetic_block_pipeline.hpp"
#include "backend/mul_split.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <set>
#include <sstream>

namespace pred::beir::arithmetic {
namespace {
void require(bool condition,const std::string& reason) {
    if(!condition)throw std::runtime_error("block pipeline: "+reason);
}
std::string identity(const ConditionalViews& v) {
    std::ostringstream s;s<<v.candidate_policy<<'/'<<v.complete<<'/'<<v.assignments<<'/'<<v.structures;
    for(const auto& x:v.views){s<<'/'<<x.id<<'/'<<x.candidate_id<<'/'<<rtlzz::arithmeticHash(emitText(x.graph));for(int at:x.placement)s<<':'<<at;}
    return rtlzz::arithmeticHash(s.str());
}
}

llvm::json::Object blockPipelinePolicyJson(const BlockPipelinePolicy& p) {
    llvm::json::Array widths;for(int w:p.block_widths)widths.push_back(w);
    return llvm::json::Object{{"version",kBlockPipelinePolicy},{"block_widths",std::move(widths)},
        {"max_structures",std::uint64_t(p.max_structures)},{"max_views",std::uint64_t(p.max_views)},
        {"max_assignments",std::uint64_t(p.max_assignments)},
        {"placement","all dependency-respecting assignments of indivisible scheduling units within finite limits"},
        {"deduplication","exact structural graph and exact placement; no timing or price pruning"},
        {"model_coverage_granted",false},{"global_architecture_optimum_claimed",false}};
}

BlockPipelineInventory enumerateBlockPipelineViews(const Program& source,const pipeline::Options& options,
    const BlockPipelinePolicy& policy) {
    require(options.arithmetic_request&&options.arithmetic_request->cost_mode=="fixed",
            "separate fixed structural context required");
    require(!policy.block_widths.empty()&&policy.block_widths.size()<=16,
            "one to sixteen distinct structural widths required");
    require(policy.max_structures&&policy.max_structures<=64&&policy.max_views&&policy.max_views<=65536
            &&policy.max_assignments&&policy.max_assignments<=1000000,"finite structural/placement budgets required");
    auto widths=policy.block_widths;std::sort(widths.begin(),widths.end());
    require(std::adjacent_find(widths.begin(),widths.end())==widths.end(),"duplicate block width");
    for(int w:widths)require(w==0||(w>=2&&w<=128),"block width must be zero or 2..128");
    BlockPipelineInventory result;result.policy=policy;auto& inventory=result.conditional;
    inventory.candidate_policy=kBlockPipelinePolicy;
    std::vector<const ArithmeticRegion*> regions;
    for(const auto& r:source.arithmetic_regions)if(r.template_version=="mul-original-v1")regions.push_back(&r);
    std::sort(regions.begin(),regions.end(),[](auto a,auto b){return a->members.front()<b->members.front();});
    std::vector<mul_split::Configuration> configuration;std::vector<std::string> labels;
    std::set<std::string> graphs;bool stop=false;
    auto one=[&]() {
        auto graph=mul_split::expand(source,configuration);
        std::string candidate="candidate";for(const auto& label:labels)candidate+="/"+label;
        const auto fingerprint=rtlzz::arithmeticHash(emitText(graph));
        if(!graphs.insert(fingerprint).second){++result.duplicate_structures;return;}
        if(inventory.structures>=policy.max_structures){result.structures_complete=false;stop=true;
            inventory.reason="declared structure budget exhausted";return;}
        ++inventory.structures;
        auto baseline=pipeline::build(graph,options);graph=std::move(baseline.combinational);
        const auto units=pipeline::diagnosticSchedulingUnits(graph,options);
        auto trial=baseline.initial_stages;
        std::vector<std::size_t> owner(graph.signals.size(),std::numeric_limits<std::size_t>::max());
        for(std::size_t i=0;i<units.size();++i)for(auto n:units[i].members)owner[n]=i;
        std::set<std::vector<int>> legal;
        std::function<void(std::size_t)> place=[&](std::size_t i) {
            if(stop)return;
            if(i==units.size()){
                if(inventory.assignments>=policy.max_assignments){result.placements_complete=false;stop=true;
                    inventory.reason="declared complete-assignment budget exhausted";return;}
                ++inventory.assignments;
                if(std::isfinite(pipeline::evaluateSchedule(graph,options,trial).final_delay)){
                    if(inventory.views.size()+legal.size()>=policy.max_views){result.placements_complete=false;stop=true;
                        inventory.reason="declared legal-placement budget exhausted";return;}
                    legal.insert(trial);
                }
                return;
            }
            const auto& unit=units[i];int lower=0;
            if(!unit.constant)for(auto n:unit.members)for(const auto& operand:graph.signal(n).driver->operands){
                if(operand.kind!=OperandKind::Symbol||owner[operand.node]==i)continue;
                const auto& value=graph.signal(operand.node).value;
                if(value.valid&&value.constant)continue;
                require(owner[operand.node]<i,"scheduling units are not topologically ordered");
                lower=std::max(lower,trial[operand.node]);
            }
            if(unit.fixed||unit.constant){
                const int at=baseline.initial_stages[unit.members.front()];
                if(!unit.constant&&at<lower){++result.dependency_pruned;return;}
                for(auto n:unit.members)trial[n]=at;place(i+1);return;
            }
            result.dependency_pruned+=std::size_t(std::min(lower,options.register_stages+1));
            for(int at=lower;at<=options.register_stages&&!stop;++at){
                for(auto n:unit.members)trial[n]=at;place(i+1);
            }
        };
        place(0);std::size_t n=0;
        for(auto& placement:legal)inventory.views.push_back({candidate+"/view"+std::to_string(n++),candidate,graph,placement});
    };
    std::function<void(std::size_t)> expand=[&](std::size_t i) {
        if(stop)return;
        if(i==regions.size()){
            try{one();}catch(const rtlzz::ArithmeticError& e){
                // Only a template's declared structural ineligibility can be
                // excluded. Missing fixed front-end costs or a materialization
                // failure must not silently shrink the candidate universe.
                if(e.diagnostic.failure_stage!="eligibility"||e.diagnostic.failure_code!="template_ineligible")throw;
                result.rejected_structures.push_back(e.what());
            }
            return;
        }
        const auto& r=*regions[i];const int maximum=std::max(r.template_parameters.at("lhs_width"),r.template_parameters.at("rhs_width"));
        for(int w:widths){
            // A full-width one-leaf expansion is structurally redundant with
            // original only when original is in this declared grammar.
            if(w&&(w>maximum||(w==maximum&&std::binary_search(widths.begin(),widths.end(),0))))continue;
            configuration.push_back({r.id,w});labels.push_back(w?"block"+std::to_string(w):"original");
            expand(i+1);labels.pop_back();configuration.pop_back();if(stop)return;
        }
    };
    expand(0);
    inventory.complete=result.structures_complete&&result.placements_complete&&!inventory.views.empty();
    if(inventory.views.empty()&&inventory.reason.empty())inventory.reason="no structurally eligible legal placement";
    inventory.inventory_sha256=identity(inventory);return result;
}
}
