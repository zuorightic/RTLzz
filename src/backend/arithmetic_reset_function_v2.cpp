#include "backend/arithmetic_reset_function.hpp"
#include <cmath>
#include <map>
#include <stdexcept>

namespace pred::beir::arithmetic {
ResetTimingPrice resetTimingPrice(const llvm::json::Object& m,double ff,double ones,double t,int depth){
    ResetTimingPrice r;
    auto refuse=[&](const std::string& why){r.reason=why;return r;};
    if(!std::isfinite(ff)||!std::isfinite(ones)||!std::isfinite(t)||ff<=0||ones<0||ones>ff||std::floor(ff)!=ff||std::floor(ones)!=ones||t<=0)return refuse("invalid reset state counts or target");
    if(depth!=1)return refuse("reset native depth outside qualified domain");
    auto interval=m.getArray("target_interval_ps");auto support=m.getObject("numeric_support");auto names=m.getArray("features");auto coefficients=m.getArray("coefficients");auto guard=m.getNumber("upper_guard_ps");
    if(m.getString("version")!="common-reset-timing-function-v2"||!interval||interval->size()!=2||!support||!names||!coefficients||names->size()!=coefficients->size()||!guard||!std::isfinite(*guard)||*guard<0)throw std::runtime_error("invalid reset function model");
    auto number=[](const llvm::json::Value& v){auto n=v.getAsNumber();if(!n||!std::isfinite(*n))throw std::runtime_error("invalid reset model number");return *n;};
    if(t<number((*interval)[0])||t>number((*interval)[1]))return refuse("reset target outside numerical interval");
    std::map<std::string,double> f={{"one",1.},{"sqrt_total_FF_bits",std::sqrt(ff)},{"sqrt_reset_one_bits",std::sqrt(ones)},{"clock_relaxation",1.-500./t},{"total_FF_bits",ff},{"reset_one_bits",ones}};
    for(const auto& item:*support){auto n=item.first.str();auto bounds=item.second.getAsArray();if(!f.count(n)||!bounds||bounds->size()!=2)throw std::runtime_error("invalid reset model support");if(f.at(n)<number((*bounds)[0])||f.at(n)>number((*bounds)[1]))return refuse("reset numerical feature outside support: "+n);}
    double point=0;
    for(std::size_t i=0;i<names->size();++i){auto name=(*names)[i].getAsString();double c=number((*coefficients)[i]);if(!name||!f.count(name->str())||c<0)throw std::runtime_error("invalid reset model coefficient");point+=c*f.at(name->str());}
    r.covered=true;r.point_intrinsic_ps=point;r.guard_intrinsic_ps=point+*guard;r.required_period_ps=r.guard_intrinsic_ps/.8;r.violates=r.guard_intrinsic_ps>.8*t;return r;
}
}
