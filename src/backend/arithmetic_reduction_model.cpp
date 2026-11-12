#include "backend/arithmetic_reduction_model.hpp"
#include <llvm/Support/MemoryBuffer.h>
#include <algorithm>
#include <cmath>
#include <set>
namespace pred::beir::arithmetic {
namespace {
void require(bool b,const std::string& s){if(!b)throw std::runtime_error(s);}
double real(const llvm::json::Value& v){auto n=v.getAsNumber();require(n&&std::isfinite(*n),"invalid G model number");return *n;}
}
struct ReductionCostModel::Impl {llvm::json::Value model{nullptr};};
ReductionCostModel::ReductionCostModel(const std::string& path,const std::string& hash,const std::string& profile,ReductionUse use):impl_(std::make_shared<Impl>()){
    auto bytes=llvm::MemoryBuffer::getFile(path);require(bool(bytes),"missing G reduction model");auto raw=(*bytes)->getBuffer().str();require(rtlzz::arithmeticHash(raw)==hash,"G model SHA mismatch");
    auto v=llvm::json::parse(raw);if(!v)throw std::runtime_error(llvm::toString(v.takeError()));impl_->model=std::move(*v);auto m=impl_->model.getAsObject();
    require(m&&m->getString("version")=="asap7-G-reduction-function-v1"&&m->getString("policy")==kReductionPolicy,"G reduction model schema differs");
    require(m->getString("feature_version")=="bit-column-features-v2-balanced-levels","G feature schema differs");
    require(m->getString("quality_status")=="passed_independent"||(use==ReductionUse::CandidateParity&&m->getString("quality_status")=="candidate"),"G reduction model is not independently qualified");
    require(m->getString("platform_profile_sha256")==profile&&m->getString("load")=="single_register_capture"&&m->getBoolean("independent_input_bits")==true,"G flow/load domain mismatch");
}
ReductionPrice ReductionCostModel::price(const ReductionRegion& r,double t) const {
    const auto& m=*impl_->model.getAsObject();auto f=reductionFeatures(r.width,r.terms,t);auto interval=m.getArray("target_interval_ps");
    require(interval&&interval->size()==2&&t>=real((*interval)[0])&&t<=real((*interval)[1]),"G target outside frozen interval");
    const auto* domain=m.getObject("numeric_support");require(domain&&!domain->empty(),"missing G support");
    for(const auto& item:*domain){auto name=item.first.str();auto range=item.second.getAsArray();require(f.count(name)&&range&&range->size()==2,"invalid G support field");double lo=real((*range)[0]),hi=real((*range)[1]),v=f.at(name),eps=1e-10*std::max({1.,std::abs(lo),std::abs(hi)});
        require(lo<=hi&&v>=lo-eps&&v<=hi+eps,"G feature outside frozen support: "+name);}
    auto predict=[&](const char* name){auto model=m.getObject(name);require(model&&model->getString("kind")=="nonnegative_linear","invalid G function kind");auto names=model->getArray("features"),coeff=model->getArray("coefficients");
        require(names&&coeff&&names->size()==coeff->size()&&!names->empty(),"invalid G coefficients");std::set<std::string> seen;double y=0;
        for(std::size_t i=0;i<names->size();++i){auto n=(*names)[i].getAsString();double c=real((*coeff)[i]);require(n&&f.count(n->str())&&seen.insert(n->str()).second&&c>=0,"invalid G feature/coefficient");y+=f.at(n->str())*c;}
        require(std::isfinite(y)&&y>0,"invalid G prediction");return y;};return {predict("delay_ps"),predict("area_um2")};
}
}
