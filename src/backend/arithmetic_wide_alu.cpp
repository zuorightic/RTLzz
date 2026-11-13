#include "backend/arithmetic_wide_alu.hpp"
#include <llvm/ADT/APInt.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <stdexcept>
#include <tuple>
namespace pred::beir::arithmetic {
WideAluGeometry demandedWideAluGeometry(const llvm::json::Object& s){
    auto integer=[&](const char* name){auto n=s.getInteger(name);if(!n)throw std::runtime_error("ALU integer required");return int(*n);};
    auto string=[&](const char* name){auto n=s.getString(name);if(!n)throw std::runtime_error("ALU string required");return n->str();};
    auto mask=[&](const char* name){auto raw=string(name);llvm::StringRef value(raw);if(value.starts_with("0x")||value.starts_with("0X"))value=value.drop_front(2);return llvm::APInt(8193,value,16);};
    const int wa=integer("lhs_width"),wb=integer("rhs_width"),w=integer("arithmetic_width"),lo=integer("result_lo"),v=integer("value_width");
    auto az=mask("lhs_zero_hex"),ao=mask("lhs_one_hex"),bz=mask("rhs_zero_hex"),bo=mask("rhs_one_hex");auto op=string("operation"),statusName=string("status"),control=string("carry");
    struct Node{int kind=-1,a=0,b=0,source=-1,bit=0;};std::vector<Node> nodes(2);std::map<std::tuple<int,int,int>,int> intern;
    auto leaf=[&](int source,int bit){int n=nodes.size();nodes.push_back({-1,0,0,source,bit});return n;};
    std::function<int(int,int,int)> gate=[&](int kind,int a,int b){
        if(kind==3){if(a<2)return 1-a;if(nodes[a].kind==3)return nodes[a].a;}
        if(kind==0){if(!a||!b)return 0;if(a==1)return b;if(b==1||a==b)return a;}
        if(kind==1){if(a==1||b==1)return 1;if(!a)return b;if(!b||a==b)return a;}
        if(kind==2){if(!a)return b;if(!b)return a;if(a==b)return 0;if(a==1)return gate(3,b,0);if(b==1)return gate(3,a,0);}
        if(kind!=3&&a>b)std::swap(a,b);auto key=std::make_tuple(kind,a,b);auto it=intern.find(key);if(it!=intern.end())return it->second;int n=nodes.size();nodes.push_back({kind,a,b});intern[key]=n;return n;
    };
    int ctrl=control=="dynamic"?leaf(2,0):int(control=="one");bool compare=op=="ult"||op=="ule"||op=="eq";
    auto needed=[&](int bit){if(compare)return true;if(op=="mux")return bit>=lo&&bit<lo+v;return bit<(statusName=="none"?lo+v:w);};
    auto inputs=[&](bool rhs){std::vector<int> out;int width=rhs?wb:wa;auto z=rhs?bz:az,o=rhs?bo:ao;
        for(int i=0;i<w;++i){if(i>=width||z[i])out.push_back(0);else if(o[i])out.push_back(1);else out.push_back(needed(i)?leaf(rhs?1:0,i):0);}return out;};
    auto aa=inputs(false),bb=inputs(true);std::vector<int> native;int flag=0;
    if(op=="mul"){
        WideAluGeometry result;result.mul=true;
        for(int i=0;i<wa;++i)if(needed(i)&&!(az|ao)[i])result.lhs_bits.push_back(i);
        for(int i=0;i<wb;++i)if(needed(i)&&!(bz|bo)[i])result.rhs_bits.push_back(i);
        auto am=llvm::APInt::getLowBitsSet(8193,wa)^az,bm=llvm::APInt::getLowBitsSet(8193,wb)^bz;int lowa=0,lowb=0;while(lowa<wa&&az[lowa])++lowa;while(lowb<wb&&bz[lowb])++lowb;int high=(am*bm).getActiveBits();bool constant=am==ao&&bm==bo;
        for(int i=0;i<v;++i)if(!constant&&lo+i>=lowa+lowb&&lo+i<high)result.dynamic_outputs.push_back(i);
        result.features={{"alu_xor",0},{"alu_and",0},{"alu_or",0},{"alu_not",0},{"alu_work",0},{"alu_depth",0}};return result;
    }
    if(op=="mux")for(int i=0;i<w;++i)native.push_back(gate(1,gate(0,gate(3,ctrl,0),aa[i]),gate(0,ctrl,bb[i])));
    else if(op=="eq"){
        std::vector<int> diff;for(int i=0;i<w;++i)diff.push_back(gate(2,aa[i],bb[i]));
        while(diff.size()>1){std::vector<int> next;for(std::size_t i=0;i<diff.size();i+=2)next.push_back(gate(1,diff[i],i+1<diff.size()?diff[i+1]:0));diff=std::move(next);}
        native.assign(w,0);native[0]=gate(3,diff[0],0);
    }else{
        bool sub=op=="sub"||op=="sbb"||op=="ult"||op=="ule";if(op=="ule")std::swap(aa,bb);if(sub){for(auto& bit:bb)bit=gate(3,bit,0);ctrl=(op=="sub"||op=="sbb")?gate(3,ctrl,0):1;}
        std::vector<int> p(w),g(w);for(int i=0;i<w;++i)p[i]=gate(2,aa[i],bb[i]);auto original=p;for(int i=0;i<w;++i)g[i]=gate(0,aa[i],bb[i]);g[0]=gate(1,g[0],gate(0,p[0],ctrl));int levels=int(std::ceil(std::log2(w)));
        for(int i=1;i<=levels;++i)for(int j=(1<<i)-1;j<w;j+=1<<i){g[j]=gate(1,g[j],gate(0,p[j],g[j-(1<<(i-1))]));p[j]=gate(0,p[j],p[j-(1<<(i-1))]);}
        for(int i=levels;i>0;--i)for(int j=(1<<i)+(1<<(i-1))-1;j<w;j+=1<<i){g[j]=gate(1,g[j],gate(0,p[j],g[j-(1<<(i-1))]));p[j]=gate(0,p[j],p[j-(1<<(i-1))]);}
        for(int i=0;i<w;++i)native.push_back(gate(2,original[i],i?g[i-1]:ctrl));if(op=="ult"||op=="ule"){native.assign(w,0);native[0]=op=="ult"?gate(3,g.back(),0):g.back();}flag=sub?gate(3,g.back(),0):g.back();
    }
    std::vector<int> outputs(native.begin()+lo,native.begin()+lo+v);if(statusName!="none")outputs.push_back(flag);
    std::set<int> live,inputsUsed;std::vector<int> depth(nodes.size());
    std::function<int(int)> visit=[&](int n){if(n<2)return 0;if(nodes[n].kind<0){inputsUsed.insert(n);return 0;}if(live.count(n))return depth[n];live.insert(n);return depth[n]=std::max(visit(nodes[n].a),visit(nodes[n].b))+1;};for(int n:outputs)visit(n);
    WideAluGeometry result;for(int n:inputsUsed){if(nodes[n].source==0)result.lhs_bits.push_back(nodes[n].bit);if(nodes[n].source==1)result.rhs_bits.push_back(nodes[n].bit);}std::sort(result.lhs_bits.begin(),result.lhs_bits.end());std::sort(result.rhs_bits.begin(),result.rhs_bits.end());
    int maximum=0;for(std::size_t i=0;i<outputs.size();++i){if(outputs[i]>=2)result.dynamic_outputs.push_back(i);maximum=std::max(maximum,depth[outputs[i]]);}
    result.features={{"alu_xor",0},{"alu_and",0},{"alu_or",0},{"alu_not",0},{"alu_work",double(live.size())},{"alu_depth",double(maximum)}};const char* names[]={"alu_and","alu_or","alu_xor","alu_not"};for(int n:live)++result.features[names[nodes[n].kind]];return result;
}
}
