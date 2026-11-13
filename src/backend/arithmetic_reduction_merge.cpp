#include "backend/arithmetic_reduction_merge.hpp"
#include <algorithm>
#include <cmath>
namespace pred::beir::arithmetic {
std::map<std::string,double> reductionMergeFeatures(int width,const std::vector<std::vector<ReductionBit>>& terms,double target) {
    auto result=reductionFeatures(width,terms,target);std::vector<int> heights(width,0);
    for(const auto& term:terms)for(auto bit:term)++heights[bit.column];
    while(*std::max_element(heights.begin(),heights.end())>2){
        std::vector<int> next(width,0);
        for(int i=0;i<width;++i){const int counters=heights[i]/3;next[i]+=heights[i]%3+counters;if(i+1<width)next[i+1]+=counters;}
        heights=std::move(next);
    }
    int full=0,half=0,increment=0,wire=0,segments=0,maximum=0,span=0;bool carry=false;
    for(int h:heights){
        if(h==2){if(carry)++full;else{++half;++segments;}carry=true;++span;}
        else if(h==1&&carry){++increment;++span;}
        else{if(h==1||carry)++wire;carry=false;maximum=std::max(maximum,span);span=0;}
        maximum=std::max(maximum,span);
    }
    result["post_merge_full_adder_columns"]=full;result["post_merge_half_adder_columns"]=half;
    result["post_merge_increment_columns"]=increment;result["post_merge_wire_columns"]=wire;
    result["post_merge_carry_segments"]=segments;result["post_merge_max_carry_span"]=maximum;
    result["log2_post_merge_carry_span"]=std::log2(double(maximum)+1.);
    result["post_merge_pressure_work"]=(result.at("counter_count")+full+half+increment)*1500./target;
    return result;
}
}
