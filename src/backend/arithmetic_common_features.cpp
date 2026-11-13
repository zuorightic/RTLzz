#include "backend/arithmetic_common_features.hpp"
#include <llvm/ADT/APInt.h>
#include <algorithm>
#include <cmath>
#include <set>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;
const O& object(const O& o,const char* n){auto p=o.getObject(n);if(!p)throw std::runtime_error("missing common feature object");return *p;}
double number(const O& o,const std::string& n){auto v=o.getNumber(n);if(!v||!std::isfinite(*v))throw std::runtime_error("invalid common numerical field: "+n);return *v;}
llvm::APInt mask(const O& o,const std::string& n,unsigned width){
    auto p=o.getString(n);if(!p)return llvm::APInt(width,0);auto s=*p;
    if(s.substr(0,2)=="0x"||s.substr(0,2)=="0X")s=s.drop_front(2);
    if(s.empty()||s.size()>4096||s.find_first_not_of("0123456789abcdefABCDEF")!=llvm::StringRef::npos)throw std::runtime_error("invalid common known-bit mask");
    llvm::APInt full(std::max<unsigned>(width,unsigned(s.size()*4)),s,16);
    if(full.getActiveBits()>width)throw std::runtime_error("common mask outside width");return full.trunc(width);
}
}
std::map<std::string,double> commonOperatorFeatures(const O& q){
    auto f=operatorQueryFeatures(q);
    f["width_result_product"]=std::pow(f.at("result_width"),2);
    f["lhs_result_product"]=f.at("lhs_width")*f.at("result_width");f["rhs_result_product"]=f.at("rhs_width")*f.at("result_width");
    const double al=f.at("lhs_zero_lsb_run"),bl=f.at("rhs_zero_lsb_run"),ah=f.at("lhs_width")-f.at("lhs_zero_msb_run"),bh=f.at("rhs_width")-f.at("rhs_zero_msb_run"),r=f.at("result_width");
    f["lhs_active_bits"]=f.at("lhs_width")-f.at("lhs_zero_count");f["rhs_active_bits"]=f.at("rhs_width")-f.at("rhs_zero_count");f["active_width_product"]=f.at("lhs_active_bits")*f.at("rhs_active_bits");
    f["add_overlap_bits"]=std::max(0.,std::min({ah,bh,r})-std::max(al,bl));f["add_carry_span"]=std::max(0.,std::min(std::max(ah,bh)+1,r)-std::max(al,bl));f["log2_add_carry_span"]=std::log2(std::max(1.,f.at("add_carry_span")));
    double terms=0;for(int i=int(al);i<int(ah);++i)terms+=std::max(0.,std::min(bh,r-i)-bl);f["retained_product_terms"]=terms;
    for(auto name:{"lhs","rhs","result"})f[std::string("log2_")+name]=std::log2(std::max(1.,f.at(std::string(name)+"_width")));
    for(auto name:{"consumer_lhs_active_bits","consumer_rhs_active_bits","consumer_lhs_zero_lsb_run","consumer_rhs_zero_lsb_run","consumer_lhs_zero_msb_run","consumer_rhs_zero_msb_run","consumer_add_overlap_bits","consumer_add_carry_span","consumer_known_one_bits","consumer_nonzero_carry"})f[name]=0;
    auto consumers=object(q,"local_context").getArray("consumers");if(!consumers)throw std::runtime_error("missing common consumers");
    for(const auto& raw:*consumers){auto c=raw.getAsObject();if(!c)throw std::runtime_error("invalid common consumer");const auto& s=object(*c,"spec");int result=int(number(s,"result_width")),ends[2][2],index=0,ones=0;
        for(auto prefix:{std::string("lhs"),std::string("rhs")}){
            int width=int(number(s,prefix+"_width"));if(width<1||width>257||result<1||result>257)throw std::runtime_error("common consumer dimensions");int live=std::min(width,result),low=0,high=0,known=0,zeroes=0;auto zero=mask(s,prefix+"_zero_hex",width),one=mask(s,prefix+"_one_hex",width);
            if(!(zero&one).isZero())throw std::runtime_error("contradictory common consumer mask");for(int i=0;i<live;++i){zeroes+=zero[i];known+=one[i];}
            while(low<live&&zero[low])++low;while(high<live&&zero[live-1-high])++high;ones+=known;
            f["consumer_"+prefix+"_active_bits"]=std::max(f["consumer_"+prefix+"_active_bits"],double(live-zeroes-known));
            f["consumer_"+prefix+"_zero_lsb_run"]=std::max(f["consumer_"+prefix+"_zero_lsb_run"],double(low));f["consumer_"+prefix+"_zero_msb_run"]=std::max(f["consumer_"+prefix+"_zero_msb_run"],double(high));ends[index][0]=low;ends[index++][1]=live-high;
        }
        f["consumer_add_overlap_bits"]=std::max(f["consumer_add_overlap_bits"],double(std::max(0,std::min({ends[0][1],ends[1][1],result})-std::max(ends[0][0],ends[1][0]))));
        f["consumer_add_carry_span"]=std::max(f["consumer_add_carry_span"],double(std::max(0,std::min(std::max(ends[0][1],ends[1][1])+1,result)-std::max(ends[0][0],ends[1][0]))));f["consumer_known_one_bits"]=std::max(f["consumer_known_one_bits"],double(ones));
        auto carry=s.getString("carry").value_or("zero");if(carry!="zero"&&carry!="one"&&carry!="dynamic")throw std::runtime_error("invalid common consumer carry");f["consumer_nonzero_carry"]=std::max(f["consumer_nonzero_carry"],double(carry!="zero"));
    }
    f["target_pressure"]=1500./f.at("target_period_ps");f["pressure_product_terms"]=terms*f.at("target_pressure");f["pressure_add_span"]=f.at("add_carry_span")*f.at("target_pressure");return f;
}
std::map<std::string,double> logicalInputLoadFeatures(const std::vector<int>& widths,const std::vector<std::vector<int>>& sinks){
    if(widths.empty())throw std::runtime_error("missing common inputs");std::size_t n=0;for(int w:widths){if(w<=0)throw std::runtime_error("invalid common input width");n+=std::size_t(w);}std::vector<unsigned> counts(n,0);
    for(const auto& sink:sinks){if(sink.empty())throw std::runtime_error("empty common sink");std::set<int> distinct;for(int bit:sink){if(bit<0||std::size_t(bit)>=n)throw std::runtime_error("common input bit outside range");distinct.insert(bit);}for(int bit:distinct)++counts[bit];}
    std::map<std::string,double> f{{"input_logical_uses",0},{"input_loaded_bits",0},{"input_max_bit_load",0},{"input_log_load_sum",0},{"input_sqrt_load_sum",0}};
    for(unsigned c:counts){f["input_logical_uses"]+=c;f["input_loaded_bits"]+=c>0;f["input_max_bit_load"]=std::max(f["input_max_bit_load"],double(c));f["input_log_load_sum"]+=std::log2(c+1.);f["input_sqrt_load_sum"]+=std::sqrt(double(c));}return f;
}
double commonNumericalPrediction(const O& model,const std::map<std::string,double>& f){
    auto names=model.getArray("features"),co=model.getArray("coefficients");auto kind=model.getString("kind");if(!names||!co||names->size()!=co->size()||!kind||(*kind!="linear"&&*kind!="nonnegative_linear"))throw std::runtime_error("invalid common predictor");double out=model.getNumber("intercept").value_or(0.);std::set<std::string> seen;
    for(std::size_t i=0;i<names->size();++i){auto n=(*names)[i].getAsString();auto c=(*co)[i].getAsNumber();if(!n||!c||!std::isfinite(*c)||!seen.insert(n->str()).second||(*kind=="nonnegative_linear"&&*c<0))throw std::runtime_error("invalid common coefficient");out+=f.at(n->str())* *c;}
    if(!std::isfinite(out))throw std::runtime_error("common prediction overflow");return std::max(0.,out);
}
}
