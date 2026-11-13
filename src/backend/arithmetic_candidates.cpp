#include "backend/arithmetic_selection.hpp"
#include "backend/arithmetic_core.hpp"
#include "backend/mul_split.hpp"
#include <algorithm>
#include <set>

namespace pred::beir::arithmetic {
CandidateEnumeration visitCandidateGraphs(const Program& source,const rtlzz::ArithmeticRequest& request,
    const std::function<void(const Program&,const std::string&,const std::string&)>& visitor,
    const std::function<void(const std::string&,const rtlzz::ArithmeticDiagnostic&)>& rejected) {
    if(request.candidate_policy=="mul-width-tree-cuts-v1")
        throw rtlzz::ArithmeticError({"eligibility","candidate_policy_requires_block_inventory",
            "mul-width-tree-cuts-v1 requires enumerateBlockPipelineViews and its explicit completeness/budget report",{},{}});
    std::vector<const ArithmeticRegion*> regions;
    for(const auto& r:source.arithmetic_regions)if(r.template_version=="mul-original-v1")regions.push_back(&r);
    if(regions.empty()&&request.candidate_policy=="mul-template-v1")throw rtlzz::ArithmeticError({"eligibility","template_ineligible","no eligible original Mul occurrence",{},{}});
    std::sort(regions.begin(),regions.end(),[](auto a,auto b){return a->members[0]<b->members[0];});
    CandidateEnumeration result;std::set<std::string> shapes;
    std::vector<mul_split::Configuration> configs;std::vector<std::string> labels;
    std::function<void(std::size_t)> enumerate=[&](std::size_t i) {
        if(result.unique>=16)return;
        if(i==regions.size()) {
            std::string id="candidate";for(const auto& label:labels)id+="/"+label;
            Program graph;
            try{graph=mul_split::expand(source,configs);}
            catch(const rtlzz::ArithmeticError& e){if(rejected)rejected(id,e.diagnostic);return;}
            ++result.eligible;auto hash=rtlzz::arithmeticHash(emitText(graph));
            if(!shapes.insert(hash).second)return;
            ++result.unique;visitor(graph,id,hash);return;
        }
        const auto& r=*regions[i];int width=std::max(r.template_parameters.at("lhs_width"),r.template_parameters.at("rhs_width"));
        if(request.candidate_policy=="mul-template-v1") {
            configs.push_back({r.id,request.template_parameters.at("block_width")});labels.push_back("block"+std::to_string(configs.back().block_width));
            enumerate(i+1);configs.pop_back();labels.pop_back();return;
        }
        for(const auto& descriptor:mul_split::registry()) {
            std::string mode=descriptor.id;
            if(request.candidate_policy=="core-original-v1"&&mode!="original")continue;
            int block=descriptor.block_divisor?std::max(2,width/descriptor.block_divisor):0;
            if(block&&(block>width||r.template_parameters.at("result_width")>r.template_parameters.at("lhs_width")+r.template_parameters.at("rhs_width")))continue;
            if(mode=="quarter"&&block==std::max(2,width/2))continue;
            configs.push_back({r.id,block});labels.push_back(mode);enumerate(i+1);configs.pop_back();labels.pop_back();
        }
    };
    enumerate(0);return result;
}
}
