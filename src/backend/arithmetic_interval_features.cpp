#include "backend/arithmetic_interval_features.hpp"
#include <llvm/ADT/APInt.h>
#include <algorithm>
#include <cmath>
#include <numeric>
#include <stdexcept>

namespace pred::beir::arithmetic {
namespace {
using O=llvm::json::Object;
const O& object(const O& o,const char* k){auto p=o.getObject(k);if(!p)throw std::runtime_error("missing interval object");return *p;}
int integer(const O& o,const std::string& k){auto n=o.getInteger(k);if(!n||*n<1||*n>257)throw std::runtime_error("invalid interval width");return int(*n);}
llvm::APInt mask(const O& o,const std::string& key,int width){
    auto s=o.getString(key).value_or("0x0");if(s.starts_with("0x")||s.starts_with("0X"))s=s.drop_front(2);
    if(s.empty()||s.size()>4096||s.find_first_not_of("0123456789abcdefABCDEF")!=llvm::StringRef::npos)throw std::runtime_error("invalid interval mask");
    llvm::APInt v(std::max<unsigned>(width,unsigned(s.size()*4)),s,16);if(v.getActiveBits()>unsigned(width))throw std::runtime_error("interval mask outside operand");return v.trunc(width);
}
std::vector<bool> live(const O& spec,const std::string& prefix,int r){
    int w=integer(spec,prefix+"_width");auto zero=mask(spec,prefix+"_zero_hex",w),one=mask(spec,prefix+"_one_hex",w);
    if(!(zero&one).isZero())throw std::runtime_error("conflicting interval masks");std::vector<bool> out(r,false);for(int i=0;i<std::min(w,r);++i)out[i]=!zero[i]&&!one[i];return out;
}
}
std::map<std::string,double> intervalOperatorFeatures(const O& q){
    auto f=commonOperatorFeatures(q);const auto& local=object(q,"local_context");const auto& p=object(local,"producer");int r=integer(p,"result_width");auto a=live(p,"lhs",r),b=live(p,"rhs",r);
    int full=0,half=0,increment=0,wire=0,segments=0,maximum=0,span=0;bool carry=false;
    for(int i=0;i<r;++i){int h=int(a[i])+int(b[i]);if(h==2){if(carry)++full;else{++half;++segments;}carry=true;++span;}else if(h==1&&carry){++increment;++span;}else{wire+=h==1||carry;carry=false;span=0;}maximum=std::max(maximum,span);}
    f["add_full_adder_columns"]=full;f["add_half_adder_columns"]=half;f["add_increment_columns"]=increment;f["add_wire_columns"]=wire;f["add_carry_segments"]=segments;f["add_max_live_carry_span"]=maximum;f["log2_add_live_carry_span"]=std::log2(maximum+1.);
    int al=r,ah=-1,bl=r,bh=-1;for(int i=0;i<r;++i){if(a[i]){al=std::min(al,i);ah=i;}if(b[i]){bl=std::min(bl,i);bh=i;}}
    f["live_lhs_bits"]=std::count(a.begin(),a.end(),true);f["live_rhs_bits"]=std::count(b.begin(),b.end(),true);f["log2_live_lhs_bits"]=std::log2(f["live_lhs_bits"]+1.);f["log2_live_rhs_bits"]=std::log2(f["live_rhs_bits"]+1.);
    int overlap=0;for(int i=0;i<r;++i)overlap+=a[i]&&b[i];f["live_operand_overlap"]=overlap;f["live_operand_gap"]=std::max(0,std::max(ah<0?0:al,bh<0?0:bl)-std::min(ah,bh)-1);
    double uses=0,overlap_load=0,below=0,above=0,position=0;auto consumers=local.getArray("consumers");if(!consumers)throw std::runtime_error("missing interval consumers");
    for(const auto& raw:*consumers){auto c=raw.getAsObject();if(!c)throw std::runtime_error("invalid interval consumer");const auto& spec=object(*c,"spec");int width=integer(spec,"result_width");auto bindings=c->getArray("bindings");if(!bindings)throw std::runtime_error("missing interval bindings");
        for(const auto& raw_binding:*bindings){auto binding=raw_binding.getAsObject();if(!binding)throw std::runtime_error("invalid interval binding");auto index=binding->getInteger("operand_index");if(!index||(*index!=0&&*index!=1))throw std::runtime_error("invalid interval operand");auto opposite=live(spec,*index==0?"rhs":"lhs",width);auto mapping=binding->getArray("result_bit_map");if(!mapping)throw std::runtime_error("missing interval map");
            for(int bit=0;bit<std::min<int>(width,mapping->size());++bit){if(!(*mapping)[bit].getAsInteger())continue;++uses;overlap_load+=opposite[bit];for(int j=0;j<width;++j){below+=j<bit&&opposite[j];above+=j>bit&&opposite[j];}position+=double(bit)/std::max(1,width-1);}
        }
    }
    f["consumer_pin_uses"]=uses;f["consumer_overlap_pin_uses"]=overlap_load;f["consumer_carry_below_mean"]=below/std::max(1.,uses);f["consumer_carry_above_mean"]=above/std::max(1.,uses);f["consumer_relative_position_mean"]=position/std::max(1.,uses);
    f["pressure_add_logic"]=(full+half+increment)*f.at("target_pressure");f["pressure_consumer_pin_uses"]=uses*f.at("target_pressure");return f;
}
std::map<std::string,double> stateControlLoadFeatures(const std::vector<int>& inputs,const std::vector<int>& stages,int external,int mode){
    if(inputs.empty()||stages.empty()||external<0||mode<0||mode>2||std::any_of(inputs.begin(),inputs.end(),[](int x){return x<=0;})||std::any_of(stages.begin(),stages.end(),[](int x){return x<0;}))throw std::runtime_error("explicit state control domains required");
    int data=std::accumulate(inputs.begin(),inputs.end(),0),launch=stages[0]+1;std::vector<int> native{1};for(std::size_t i=1;i<stages.size();++i)native.push_back(stages[i]+1);
    auto service=mode==1?std::vector<int>{data+1}:inputs;if(mode==0)service.push_back(1);int max_native=*std::max_element(native.begin(),native.end());
    return {{"service_enable_max_load",*std::max_element(service.begin(),service.end())},{"service_enable_domains",double(service.size())},{"launch_valid_logical_load",launch},{"native_valid_max_logical_load",max_native},{"native_valid_total_logical_load",std::accumulate(native.begin(),native.end(),0)},{"native_enable_max_logical_load",*std::max_element(stages.begin(),stages.end())},{"native_enable_domains",double(stages.size())},{"control_log2_launch_load",std::log2(launch+1.)},{"control_log2_native_valid_load",std::log2(max_native+1.)},{"external_valid_logical_load",1.},{"root_reset_logical_load",double(data+1+std::accumulate(stages.begin(),stages.end(),0)+stages.size()+external+1)}};
}
}
