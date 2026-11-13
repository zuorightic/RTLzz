#include "backend/arithmetic_wide_reduction.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;
double value(const llvm::json::Value& v){auto n=v.getAsNumber();if(!n||!std::isfinite(*n))throw std::runtime_error("finite numeric value required");return *n;}
std::string supported(const O& s,const std::map<std::string,double>& f,double target){
    if(s.getString("version")!="numerical-structural-support-v2-distinct-geometry")throw std::runtime_error("wide G support version mismatch");
    auto interval=s.getArray("target_interval_ps");if(!interval||interval->size()!=2)throw std::runtime_error("missing G target interval");
    if(target<value((*interval)[0])||target>value((*interval)[1]))return "outside continuous target interval";
    auto names=s.getArray("features"),bounds=s.getArray("bounds"),scales=s.getArray("scales"),anchors=s.getArray("anchors");
    if(!names||!bounds||!scales||!anchors||names->size()!=bounds->size()||names->size()!=scales->size())throw std::runtime_error("invalid G support dimensions");
    std::vector<double> q;
    for(std::size_t i=0;i<names->size();++i){auto n=(*names)[i].getAsString();auto b=(*bounds)[i].getAsArray();
        if(!n||!b||b->size()!=2||!f.count(n->str()))throw std::runtime_error("missing G support feature");
        double x=f.at(n->str());if(x<value((*b)[0])-1e-9||x>value((*b)[1])+1e-9)return "outside structural range "+n->str();q.push_back(x);}
    std::vector<double> distances;std::set<std::vector<double>> unique;
    for(const auto& raw:*anchors){auto a=raw.getAsArray();if(!a||a->size()!=q.size())throw std::runtime_error("invalid G support anchor");
        std::vector<double> row;double sum=0;for(std::size_t i=0;i<q.size();++i){double scale=value((*scales)[i]),x=value((*a)[i]);if(scale<=0)throw std::runtime_error("invalid G support scale");row.push_back(x);sum+=std::pow((q[i]-x)/scale,2);}
        if(!unique.insert(row).second)throw std::runtime_error("duplicate G support anchor");distances.push_back(std::sqrt(sum));}
    auto k=s.getInteger("k");auto radius=s.getNumber("radius");if(!k||*k<1||std::size_t(*k)>distances.size()||!radius)throw std::runtime_error("invalid G neighborhood");
    std::sort(distances.begin(),distances.end());return distances[*k-1]>*radius+1e-9?"outside structural neighborhood":"";
}
double linear(const O& m,const std::map<std::string,double>& f){auto names=m.getArray("features"),coeff=m.getArray("coefficients");
    if(!names||!coeff||names->size()!=coeff->size())throw std::runtime_error("invalid G coefficient dimensions");
    double y=m.getNumber("intercept").value_or(0.);std::set<std::string> seen;
    for(std::size_t i=0;i<names->size();++i){auto n=(*names)[i].getAsString();if(!n||!seen.insert(n->str()).second||!f.count(n->str()))throw std::runtime_error("invalid G coefficient feature");y+=f.at(n->str())*value((*coeff)[i]);}
    if(!std::isfinite(y)||y<=0)throw std::runtime_error("invalid G intrinsic price");return y;
}
}
WideReductionFeatures wideReductionFeatures(const O& q,double target){
    auto terms=q.getArray("terms");if(!terms||terms->size()>4096)throw std::runtime_error("bounded wide G terms required");
    std::size_t bits=0;for(const auto& term:*terms){auto a=term.getAsArray();if(!a)throw std::runtime_error("G term required");bits+=a->size();}
    if(bits>1048576)throw std::runtime_error("wide G input-bit capacity exceeded");
    auto area=livePrefixReductionFeatures(q,target);int width=int(*q.getInteger("output_bits"));std::vector<int> h(width);
    for(const auto& term:*terms)for(const auto& bit:*term.getAsArray())++h[int(*(*bit.getAsArray())[2].getAsInteger())];
    int maximum=*std::max_element(h.begin(),h.end()),total=0,low=width,high=-1,first=width;
    for(int i=0;i<width;++i){total+=h[i];if(h[i]){low=std::min(low,i);high=i;}if(h[i]>=2)first=std::min(first,i);}
    int counters=0,rounds=0;
    while(*std::max_element(h.begin(),h.end())>2){std::vector<int> next(width);for(int i=0;i<width;++i){int n=h[i]/3;counters+=n;next[i]+=h[i]%3+n;if(i+1<width)next[i+1]+=n;}h=std::move(next);++rounds;}
    int rows=maximum,balanced=0;while(rows>2){rows=2*(rows/3)+rows%3;++balanced;}
    int full=0,half=0,increment=0,wire=0,segments=0,maxspan=0,span=0;bool active=false;
    for(int n:h){if(n==2){if(active)++full;else{++half;++segments;}active=true;++span;}else if(n==1&&active){++increment;++span;}else{if(n==1||active)++wire;active=false;span=0;}maxspan=std::max(maxspan,span);}
    double carry=std::max(0,high-first+1);
    std::map<std::string,double> d{{"one",1.},{"output_bits",double(width)},{"term_count",double(terms->size())},{"active_bits",double(total)},
        {"max_column_height",double(maximum)},{"counter_count",double(counters)},{"compressor_depth",double(rounds)},
        {"compression_levels",double(balanced)},{"carry_span",carry},{"log2_carry_span",std::log2(carry+1.)},{"active_span",double(high-low+1)},
        {"clock_pressure",1500./target},{"log2_target",std::log2(target/500.)},{"pressure_work",(counters+carry)*1500./target},
        {"post_merge_full_adder_columns",double(full)},{"post_merge_half_adder_columns",double(half)},{"post_merge_increment_columns",double(increment)},
        {"post_merge_wire_columns",double(wire)},{"post_merge_carry_segments",double(segments)},{"post_merge_max_carry_span",double(maxspan)},
        {"log2_post_merge_carry_span",std::log2(maxspan+1.)},{"post_merge_pressure_work",(counters+full+half+increment)*1500./target}};
    return {std::move(area),std::move(d)};
}
O evaluateWideReduction(const O& model,const O& q,bool diagnostic,bool extrapolate){try{
    if(model.getString("version")!="wide-prefix-reuse-diagnostic-v1"||model.getString("feature_version")!="wide-reduction-paired-features-v1")throw std::runtime_error("wide G model identity mismatch");
    if(!diagnostic)throw std::runtime_error("wide G successor has no independent qualification");
    if(q.getString("launch_type").value_or("register")!="register"||q.getInteger("capture_banks").value_or(1)!=1||q.getInteger("consumer_count").value_or(0)!=0)throw std::runtime_error("unqualified wide G boundary");
    auto t=q.getNumber("target_ps");auto support=model.getObject("support"),area=model.getObject("area"),delay=model.getObject("delay");if(!t||!support||!area||!delay)throw std::runtime_error("wide G price inputs required");
    auto f=wideReductionFeatures(q,*t);auto reason=supported(*support,f.area,*t);
    if(!reason.empty()&&(!extrapolate||*t<500||*t>3000))throw std::runtime_error(reason);
    return O{{"status",reason.empty()?"diagnostic_estimate":"unqualified_extrapolation"},{"operator_delay_ps",linear(*delay,f.delay)},
             {"comb_area_um2",linear(*area,f.area)},{"support_reason",reason},{"qualified_for_public_D",false}};
}catch(const std::exception& e){return O{{"status","cost_uncovered"},{"reason",std::string(e.what())}};}}
}
