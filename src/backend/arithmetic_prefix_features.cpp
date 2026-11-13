#include "backend/arithmetic_prefix_features.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>
namespace pred::beir::arithmetic {
std::map<std::string,double> livePrefixReductionFeatures(const llvm::json::Object& q,double target){
    auto widthValue=q.getInteger("output_bits");auto terms=q.getArray("terms");
    if(!widthValue||*widthValue<1||*widthValue>4096||!terms||terms->size()<3||!std::isfinite(target)||target<=0)throw std::runtime_error("invalid prefix reduction dimensions");
    int width=int(*widthValue);std::vector<int> h(width);std::set<std::pair<int64_t,int64_t>> seen;
    for(const auto& raw:*terms){auto term=raw.getAsArray();if(!term||term->empty())throw std::runtime_error("empty reduction term");std::set<int> columns;
        for(const auto& bit:*term){auto v=bit.getAsArray();if(!v||v->size()!=3)throw std::runtime_error("reduction bit triple required");auto s=(*v)[0].getAsInteger(),b=(*v)[1].getAsInteger(),c=(*v)[2].getAsInteger();
            if(!s||!b||!c||*s<0||*b<0||*c<0||*c>=width||!columns.insert(int(*c)).second||!seen.insert({*s,*b}).second)throw std::runtime_error("invalid/repeated reduction bit");++h[*c];}
    }
    int total=0;for(int n:h)total+=n;int maximum=*std::max_element(h.begin(),h.end()),counters=0,levels=0;
    while(*std::max_element(h.begin(),h.end())>2){std::vector<int> next(width);for(int i=0;i<width;++i){int n=h[i]/3;counters+=n;next[i]+=h[i]%3+n;if(i+1<width)next[i+1]+=n;}h=std::move(next);++levels;}
    struct Node{int kind=-1,a=0,b=0;};std::vector<Node> nodes(2);std::map<std::tuple<int,int,int>,int> intern;
    auto leaf=[&](){int n=nodes.size();nodes.push_back({});return n;};
    auto gate=[&](int kind,int a,int b){
        if(kind==0){if(!a||!b)return 0;if(a==1)return b;if(b==1||a==b)return a;}
        if(kind==1){if(a==1||b==1)return 1;if(!a)return b;if(!b||a==b)return a;}
        if(kind==2){if(!a)return b;if(!b)return a;if(a==b)return 0;}
        if(a>b)std::swap(a,b);auto key=std::make_tuple(kind,a,b);auto it=intern.find(key);if(it!=intern.end())return it->second;int n=nodes.size();nodes.push_back({kind,a,b});intern[key]=n;return n;
    };
    std::vector<int> aa(width),bb(width),p(width),g(width);
    for(int i=0;i<width;++i)aa[i]=h[i]?leaf():0;
    for(int i=0;i<width;++i)bb[i]=h[i]==2?leaf():0;
    for(int i=0;i<width;++i)p[i]=gate(2,aa[i],bb[i]);auto initial=p;
    for(int i=0;i<width;++i)g[i]=gate(0,aa[i],bb[i]);int before=nodes.size();int logWidth=int(std::ceil(std::log2(width)));
    for(int i=1;i<=logWidth;++i)for(int j=(1<<i)-1;j<width;j+=1<<i){g[j]=gate(1,g[j],gate(0,p[j],g[j-(1<<(i-1))]));p[j]=gate(0,p[j],p[j-(1<<(i-1))]);}
    for(int i=logWidth;i>0;--i)for(int j=(1<<i)+(1<<(i-1))-1;j<width;j+=1<<i){g[j]=gate(1,g[j],gate(0,p[j],g[j-(1<<(i-1))]));p[j]=gate(0,p[j],p[j-(1<<(i-1))]);}
    int after=nodes.size();std::vector<int> outputs;for(int i=0;i<width;++i)outputs.push_back(gate(2,initial[i],i?g[i-1]:0));
    std::vector<int> loads(nodes.size()),depth(nodes.size());std::set<int> live;
    std::function<int(int)> visit=[&](int n){++loads[n];if(nodes[n].kind<0)return 0;if(live.count(n))return depth[n];live.insert(n);return depth[n]=std::max(visit(nodes[n].a),visit(nodes[n].b))+1;};for(int n:outputs)visit(n);
    int ands=0,ors=0,xors=0,longest=0;for(int n:live){if(nodes[n].kind==2)++xors;if(n>=before&&n<after){ands+=nodes[n].kind==0;ors+=nodes[n].kind==1;}}for(int n:outputs)longest=std::max(longest,depth[n]);
    int span=0,segments=0,maxspan=0;bool carry=false;for(int n:h){if(n==2){if(!carry)++segments;carry=true;++span;}else if(n==1&&carry)++span;else{carry=false;span=0;}maxspan=std::max(maxspan,span);}
    return {{"one",1},{"output_bits",double(width)},{"term_count",double(terms->size())},{"active_bits",double(total)},{"max_column_height",double(maximum)},{"counter_count",double(counters)},{"compression_levels",double(levels)},{"log2_target",std::log2(target/500.)},{"clock_pressure",1500./target},
        {"final_live_xor_gates",double(xors)},{"final_live_prefix_and_gates",double(ands)},{"final_live_prefix_or_gates",double(ors)},{"final_live_logic_gates",double(live.size())},{"final_logic_depth",double(longest)},{"final_max_logical_fanout",double(*std::max_element(loads.begin(),loads.end()))},{"final_prefix_carry_segments",double(segments)},{"final_max_carry_span",double(maxspan)},{"log2_final_carry_span",std::log2(maxspan+1.)},{"final_pressure_logic",live.size()*1500./target},{"counter_pressure",counters*1500./target}};
}
}
