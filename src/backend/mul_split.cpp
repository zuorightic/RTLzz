#include "backend/mul_split.hpp"
#include "backend/arithmetic_core.hpp"
#include "backend/arithmetic_regions.hpp"
#include "backend/beopt_dce.hpp"
#include <algorithm>
#include <set>
namespace pred::beir::mul_split {
namespace {
Operand ref(const Program& p,NodeId id){Operand v;v.node=id;v.type=p.signal(id).type;return v;}
Operand zero(int w){Operand v;v.kind=OperandKind::Literal;v.type.width=w;v.constant.width=w;v.constant.limbs.resize((w+63)/64,0);return v;}
[[noreturn]] void reject(const std::string& id,const std::string& why){throw rtlzz::ArithmeticError({"eligibility","template_ineligible",why,id,{}});}
}
const std::vector<TemplateDescriptor>& registry(){static const std::vector<TemplateDescriptor> entries{{"original","mul-original-v1",0,"native",false},{"half","mul-block-tree-v1",2,"carry_preserving_tree",true},{"quarter","mul-block-tree-v1",4,"carry_preserving_tree",true}};return entries;}
Program expand(Program p,const std::vector<Configuration>& configurations){
    std::set<std::string> seen;
    for(const auto& config:configurations){
        if(!seen.insert(config.region_id).second)reject(config.region_id,"duplicate template configuration");
        auto it=std::find_if(p.arithmetic_regions.begin(),p.arithmetic_regions.end(),[&](const auto& r){return r.id==config.region_id;});
        if(it==p.arithmetic_regions.end()||it->template_version!="mul-original-v1")reject(config.region_id,"configuration requires a registered original Mul region");
        if(!config.block_width)continue;
        const auto old=*it;const auto anchor=old.outputs.at(0);const auto op=*p.signal(anchor).driver;
        const int aw=arithmetic::billingWidth(p,op.operands[0]),bw=arithmetic::billingWidth(p,op.operands[1]),R=p.signal(anchor).type.width,b=config.block_width;
        if(aw<1||bw<1||aw>128||bw>128||R<1||R>aw+bw||b<2||b>std::max(aw,bw))reject(old.id,"block template requires unsigned inputs <=128, 1<=R<=lhs+rhs and 2<=block<=max input width");
        ArithmeticRegion region=old;region.members.clear();region.outputs.clear();region.template_version="mul-block-tree-v1";region.sharing_policy="isolated-tree-DCE-facts-v1";
        region.internal_split_enabled=true;region.split_version="partial-products-reduction-levels-v1";region.legal_cut_groups={{}};
        region.template_parameters={{"lhs_width",aw},{"rhs_width",bw},{"result_width",R},{"block_width",b}};
        auto add=[&](int g,int width,OperationKind kind,std::vector<Operand> operands,OpCode code=OpCode::None){
            while(region.legal_cut_groups.size()<=static_cast<std::size_t>(g))region.legal_cut_groups.push_back({});
            Signal s;s.id=p.signals.size();s.name="mul_tree_"+std::to_string(anchor)+"_"+std::to_string(s.id);s.type.width=width;
            Operation d;d.kind=kind;d.op=code;d.type=s.type;d.operands=std::move(operands);s.driver=std::move(d);
            p.signals.push_back(s);region.members.push_back(s.id);region.legal_cut_groups[g].push_back(s.id);return s.id;
        };
        auto slice=[&](Operand value,int width,int lo){auto id=add(0,width,OperationKind::Slice,{value});p.signal(id).driver->hi=lo+width-1;p.signal(id).driver->lo=lo;return id;};
        auto extend=[&](int g,NodeId id,int width){if(p.signal(id).type.width==width)return id;auto n=add(g,width,OperationKind::ZExt,{ref(p,id)});p.signal(n).driver->to_width=width;return n;};
        std::vector<NodeId> terms;
        for(int ai=0;ai<aw;ai+=b)for(int bi=0;bi<bw;bi+=b){
            int xw=std::min(b,aw-ai),yw=std::min(b,bw-bi);auto a=slice(op.operands[0],xw,ai),c=slice(op.operands[1],yw,bi);
            auto product=add(0,xw+yw,OperationKind::Binary,{ref(p,a),ref(p,c)},OpCode::Mul);
            // Concatenation expresses an exact constant shift; no host-width or
            // SystemVerilog implicit expression-size dependence.
            if(ai+bi)product=add(0,xw+yw+ai+bi,OperationKind::Concat,{ref(p,product),zero(ai+bi)});
            terms.push_back(product);
        }
        int group=0;
        while(terms.size()>1){++group;std::vector<NodeId> next;
            for(std::size_t i=0;i<terms.size();i+=2){
                if(i+1==terms.size()){next.push_back(terms[i]);continue;}
                // Each sum gains one bit; even the conservative intermediate
                // bound is preserved until the single explicit final truncation.
                const int width=std::max(p.signal(terms[i]).type.width,p.signal(terms[i+1]).type.width)+1;
                auto a=extend(group,terms[i],width),c=extend(group,terms[i+1],width);
                next.push_back(add(group,width,OperationKind::AddCarry,{ref(p,a),ref(p,c),zero(1)}));
            }
            terms=std::move(next);
        }
        NodeId result=terms.at(0);int width=p.signal(result).type.width;
        if(width>R){result=add(group,R,OperationKind::Slice,{ref(p,result)});p.signal(result).driver->hi=R-1;p.signal(result).driver->lo=0;}
        else result=extend(group,result,R);
        region.outputs={result};Operation alias;alias.type=p.signal(anchor).type;alias.operands={ref(p,result)};p.signal(anchor).driver=std::move(alias);p.signal(anchor).value={};
        *it=std::move(region);
    }
    p.temp_names_initialized=false;MutableProgram graph(std::move(p));opt::eliminateDeadNodes(graph);graph.ensureValueFacts();p=graph.finish();regions::validate(p);arithmetic::validate(p,"template_expansion");return p;
}
}
