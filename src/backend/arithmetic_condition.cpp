#include "backend/arithmetic_condition.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;using A=llvm::json::Array;using F=std::map<std::string,double>;
const O& obj(const O& o,const char* key){auto p=o.getObject(key);if(!p)throw std::runtime_error(std::string("missing object ")+key);return *p;}
const A& arr(const O& o,const char* key){auto p=o.getArray(key);if(!p)throw std::runtime_error(std::string("missing array ")+key);return *p;}
std::string str(const O& o,const char* key){auto p=o.getString(key);if(!p)throw std::runtime_error(std::string("missing string ")+key);return p->str();}
double num(const O& o,const char* key){auto p=o.getNumber(key);if(!p||!std::isfinite(*p))throw std::runtime_error(std::string("invalid number ")+key);return *p;}
double infer(const O& p,const F& f,unsigned depth=0){
    if(depth>4)throw std::runtime_error("recursive predictor");
    auto kind=str(p,"kind");double value=0;
    if(kind=="linear"){
        const auto& names=arr(p,"features");const auto& coefficients=arr(p,"coefficients");
        if(names.size()!=coefficients.size())throw std::runtime_error("predictor shape mismatch");
        value=num(p,"intercept");std::set<std::string> seen;
        for(std::size_t i=0;i<names.size();++i){auto name=names[i].getAsString();auto c=coefficients[i].getAsNumber();
            if(!name||!c||!std::isfinite(*c)||!seen.insert(name->str()).second)throw std::runtime_error("invalid coefficient/feature");value+=*c*f.at(name->str());}
    }else if(kind=="segmented"){
        const auto& knots=arr(p,"knots");const auto& segments=arr(p,"segments");if(knots.size()>2||segments.size()!=knots.size()+1)throw std::runtime_error("invalid segmented model");
        std::size_t at=0;double last=-INFINITY;for(const auto& v:knots){auto k=v.getAsNumber();if(!k||!std::isfinite(*k)||*k<=last)throw std::runtime_error("invalid knots");last=*k;at+=f.at("log2_target_ps")>*k;}
        auto segment=segments[at].getAsObject();if(!segment)throw std::runtime_error("invalid segment");return infer(*segment,f,depth+1);
    }else if(kind=="width_grouped"){
        auto group=std::max(f.at("lhs_width"),f.at("rhs_width"))<=num(p,"split")?"small":"large";
        return infer(obj(obj(p,"groups"),group),f,depth+1);
    }else if(kind=="tree"){
        const auto& nodes=arr(p,"nodes");std::set<std::int64_t> seen;std::int64_t at=0;
        for(;;){if(at<0||static_cast<std::size_t>(at)>=nodes.size()||!seen.insert(at).second||seen.size()>5)throw std::runtime_error("invalid tree");auto n=nodes[at].getAsObject();if(!n)throw std::runtime_error("invalid tree node");
            if(n->get("value")){value=num(*n,"value");break;}auto next=n->getInteger(f.at(str(*n,"feature"))<=num(*n,"threshold")?"left":"right");if(!next)throw std::runtime_error("invalid child");at=*next;}
    }else throw std::runtime_error("unknown predictor kind");
    if(!std::isfinite(value))throw std::runtime_error("nonfinite prediction");return std::max(0.,value);
}
}
OperatorConditionEstimate operatorConditionEstimate(const O& model,const O& descriptor,bool allow_candidate){
    OperatorConditionEstimate result;
    try{
        auto f=operatorQueryFeatures(descriptor);const auto& flow=obj(descriptor,"flow_context");
        if(str(model,"version")!="operator-condition-function-v1"||str(flow,"profile_sha256")!=str(model,"profile_sha256")||str(flow,"boundary_policy")!=str(model,"operator_boundary_policy")||str(flow,"target_platform")!="asap7"||str(flow,"evaluation_stage")!="grt")throw std::runtime_error("model flow context mismatch");
        f["width_product"]=f.at("lhs_width")*f.at("rhs_width");f["width_result_product"]=std::pow(f.at("result_width"),2);
        f["lhs_result_product"]=f.at("lhs_width")*f.at("result_width");f["rhs_result_product"]=f.at("rhs_width")*f.at("result_width");
        const double al=f.at("lhs_zero_lsb_run"),bl=f.at("rhs_zero_lsb_run"),ah=f.at("lhs_width")-f.at("lhs_zero_msb_run"),bh=f.at("rhs_width")-f.at("rhs_zero_msb_run"),r=f.at("result_width");
        f["lhs_active_bits"]=f.at("lhs_width")-f.at("lhs_zero_count");f["rhs_active_bits"]=f.at("rhs_width")-f.at("rhs_zero_count");f["active_width_product"]=f.at("lhs_active_bits")*f.at("rhs_active_bits");
        f["add_overlap_bits"]=std::max(0.,std::min({ah,bh,r})-std::max(al,bl));f["add_carry_span"]=std::max(0.,std::min(std::max(ah,bh)+1,r)-std::max(al,bl));f["log2_add_carry_span"]=std::log2(std::max(1.,f.at("add_carry_span")));
        double terms=0;for(int i=static_cast<int>(al);i<static_cast<int>(ah);++i)terms+=std::max(0.,std::min(bh,r-i)-bl);f["retained_product_terms"]=terms;
        auto op=str(obj(obj(descriptor,"local_context"),"producer"),"operation");auto entry=obj(model,"operations").getObject(op);if(!entry)throw std::runtime_error("unknown operator");auto status=str(*entry,"status");
        if(status!="passed_independent"&&!(allow_candidate&&status=="awaiting_independent"))throw std::runtime_error("operator function unqualified");
        for(auto name:{"lhs_one_count","rhs_one_count","carry_dynamic","carry_one","consumer_mul","consumer_addcarry"})if(f.at(name))throw std::runtime_error("unsupported operand or consumer semantics");
        for(auto prefix:{std::string("lhs"),std::string("rhs")})if(f.at(prefix+"_zero_count")!=f.at(prefix+"_zero_lsb_run")+f.at(prefix+"_zero_msb_run"))throw std::runtime_error("interior known-zero pattern outside first interval domain");
        if(f.at("consumer_count")>1)throw std::runtime_error("unsupported consumer count");
        if(obj(*entry,"domain").get("local_load_family")&&(f.at("capture_banks")>2||std::max({f.at("consumer_lhs_max"),f.at("consumer_rhs_max"),f.at("consumer_result_max")})>f.at("result_width")+1))throw std::runtime_error("outside validated local load family");
        for(const auto& item:obj(obj(*entry,"domain"),"numeric_support")){auto limits=item.second.getAsArray();if(!limits||limits->size()!=2)throw std::runtime_error("invalid domain bounds");auto lo=(*limits)[0].getAsNumber(),hi=(*limits)[1].getAsNumber();if(!lo||!hi||!std::isfinite(*lo)||!std::isfinite(*hi)||*lo>*hi||f.at(item.first.str())<*lo||f.at(item.first.str())>*hi)throw std::runtime_error("outside trained numerical feature support");}
        if(auto support=obj(*entry,"domain").getObject("joint_support")){
            auto reference=f;double target=num(*support,"reference_target_ps");reference["target_period_ps"]=target;reference["abc_target_ps"]=target;reference["log2_target_ps"]=std::log2(target);
            double ratio=f.at("target_period_ps")/std::max(num(*support,"reference_floor_ps"),infer(obj(*entry,"delay"),reference));
            const auto& names=arr(*support,"features");const auto& scales=arr(*support,"scales");if(names.size()!=scales.size())throw std::runtime_error("joint support shape mismatch");
            std::vector<std::pair<double,const O*>> neighbors;
            for(const auto& value:arr(*support,"anchors")){auto a=value.getAsObject();if(!a)throw std::runtime_error("invalid support anchor");const auto& v=arr(*a,"vector");if(v.size()!=names.size())throw std::runtime_error("invalid anchor vector");double square=0;
                for(std::size_t i=0;i<names.size();++i){auto name=names[i].getAsString();auto coordinate=v[i].getAsNumber(),scale=scales[i].getAsNumber();if(!name||!coordinate||!scale||*scale<=0||!std::isfinite(*coordinate)||!std::isfinite(*scale))throw std::runtime_error("invalid support coordinate");double delta=(f.at(name->str())-*coordinate)/ *scale;square+=delta*delta;}
                neighbors.emplace_back(std::sqrt(square),a);}
            std::stable_sort(neighbors.begin(),neighbors.end(),[](auto a,auto b){return a.first<b.first;});auto count=support->getInteger("k");if(!count||*count<1||static_cast<std::size_t>(*count)>neighbors.size()||neighbors[*count-1].first>num(*support,"radius")+1e-12)throw std::runtime_error("outside joint semantic neighborhood");bool clock=false;
            for(std::int64_t i=0;i<*count;++i){const auto& a=*neighbors[i].second;clock|=ratio>=num(a,"clock_ratio_min")-1e-12&&ratio<=num(a,"clock_ratio_max")+1e-12;}if(!clock)throw std::runtime_error("outside joint neighbor clock support");
        }
        result.operator_delay_ps=infer(obj(*entry,"delay"),f);result.comb_area_um2=infer(obj(*entry,"area"),f);result.delay_reserve_ps=num(*entry,"delay_reserve_ps");
        if(result.delay_reserve_ps<0)throw std::runtime_error("negative delay reserve");result.covered=true;
    }catch(const std::exception& e){result.reason=e.what();}
    return result;
}
}
