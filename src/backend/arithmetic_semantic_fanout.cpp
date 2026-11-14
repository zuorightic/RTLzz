#include "backend/arithmetic_semantic_fanout.hpp"
#include <tuple>
#include <llvm/ADT/APInt.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <vector>

namespace pred::beir::arithmetic {
namespace {
using O = llvm::json::Object;
using A = llvm::json::Array;
using F = NumericalFeatures;
const O& object(const O& o, const char* name) {
    auto p = o.getObject(name);
    if (!p) throw std::runtime_error("flow structure object required");
    return *p;
}
double number(const llvm::json::Value& raw) {
    auto p = raw.getAsNumber();
    if (!p || !std::isfinite(*p)) throw std::runtime_error("finite flow structure number required");
    return *p;
}
int integer(const O& o, const char* name) {
    auto p = o.getInteger(name);
    if (!p) throw std::runtime_error("flow structure integer required");
    return int(*p);
}
std::string string(const O& o, const char* name) {
    auto p = o.getString(name);
    if (!p) throw std::runtime_error("flow structure string required");
    return p->str();
}
F features(const O& o) {
    F f;
    for (const auto& [name, value] : o) f[name.str()] = number(value);
    return f;
}
O json(const F& f) {
    O o;
    for (const auto& [name, value] : f) o[name] = value;
    return o;
}
void supported(const O& s, const F& f, double T) {
    if (s.getString("version")!="numerical-structural-support-v2-distinct-geometry") throw std::runtime_error("support identity");
    auto interval=s.getArray("target_interval_ps"), names=s.getArray("features"), bounds=s.getArray("bounds"), scales=s.getArray("scales"), anchors=s.getArray("anchors");
    if (!interval||interval->size()!=2||!names||!bounds||!scales||!anchors||names->size()!=bounds->size()||names->size()!=scales->size()) throw std::runtime_error("support dimensions");
    if (T<number((*interval)[0])||T>number((*interval)[1])) throw std::runtime_error("outside continuous target interval");
    std::vector<double> q;
    for (unsigned i=0;i<names->size();++i) {
        auto n=(*names)[i].getAsString(); auto range=(*bounds)[i].getAsArray();
        if (!n||!range||range->size()!=2) throw std::runtime_error("support feature");
        double v=f.at(n->str());
        if (v<number((*range)[0])-1e-9||v>number((*range)[1])+1e-9) throw std::runtime_error("outside structural range "+n->str());
        q.push_back(v);
    }
    std::vector<double> distances; std::set<std::vector<double>> distinct;
    for (const auto& raw : *anchors) {
        auto a=raw.getAsArray(); if (!a||a->size()!=q.size()) throw std::runtime_error("support anchor dimensions");
        double sum=0; std::vector<double> v;
        for (unsigned i=0;i<q.size();++i) {double x=number((*a)[i]),scale=number((*scales)[i]); if(scale<=0)throw std::runtime_error("support scale");v.push_back(x);sum+=std::pow((q[i]-x)/scale,2);}
        if (!distinct.insert(v).second) throw std::runtime_error("duplicate support anchor");
        distances.push_back(std::sqrt(sum));
    }
    auto k=s.getInteger("k"); auto radius=s.getNumber("radius");
    if (!k||*k<1||std::size_t(*k)>distances.size()||!radius) throw std::runtime_error("support neighborhood dimensions");
    std::sort(distances.begin(),distances.end());
    if (distances[*k-1]>*radius+1e-9) throw std::runtime_error("outside structural neighborhood");
}
F moments(const std::vector<int>& loads) {
    double mass=0,square=0,excess=0,shared=0,drivers=0;
    for(int n:loads)if(n>0){mass+=n;square+=double(n)*n;excess+=std::max(0,n-4);shared+=n>1;++drivers;}
    return {{"log2_mass",std::log2(mass+1.)},{"log2_concentration",std::log2(1.+square/std::max(1.,mass))},
        {"log2_effective_drivers",std::log2(1.+mass*mass/std::max(1.,square))},{"log2_repair_excess",std::log2(1.+excess)},
        {"shared_fraction",shared/std::max(1.,drivers)}};
}
struct Circuit {
    struct Node { int kind=-1, a=0, b=0; };
    std::vector<Node> nodes = std::vector<Node>(2);
    std::map<std::tuple<int,int,int>,int> intern;
    int leaf() { int n=nodes.size(); nodes.push_back({}); return n; }
    int gate(int kind,int a,int b=0) {
        if (kind==3) {
            if (a<2) return 1-a;
            if (nodes[a].kind==3) return nodes[a].a;
        } else if (kind==0) {
            if (!a||!b) return 0;
            if (a==1) return b;
            if (b==1||a==b) return a;
        } else if (kind==1) {
            if (a==1||b==1) return 1;
            if (!a) return b;
            if (!b||a==b) return a;
        } else if (kind==2) {
            if (!a) return b;
            if (!b) return a;
            if (a==b) return 0;
            if (a==1) return gate(3,b);
            if (b==1) return gate(3,a);
        } else throw std::runtime_error("unknown Boolean gate");
        if (kind!=3&&a>b) std::swap(a,b);
        auto key=std::make_tuple(kind,a,b);
        auto it=intern.find(key);
        if (it!=intern.end()) return it->second;
        int n=nodes.size(); nodes.push_back({kind,a,b}); intern[key]=n; return n;
    }
    std::pair<std::vector<int>,std::vector<int>> prefix(const std::vector<int>& a,
            const std::vector<int>& b,int carry=0) {
        if (a.empty()||a.size()!=b.size()) throw std::runtime_error("prefix dimensions");
        int w=a.size(); std::vector<int> p(w),g(w),sum(w);
        for (int i=0;i<w;++i) p[i]=gate(2,a[i],b[i]);
        auto original=p;
        for (int i=0;i<w;++i) g[i]=gate(0,a[i],b[i]);
        g[0]=gate(1,g[0],gate(0,p[0],carry));
        for (int step=1;step<w;step*=2)
            for (int j=w-1;j>=step;--j) {
                g[j]=gate(1,g[j],gate(0,p[j],g[j-step]));
                p[j]=gate(0,p[j],p[j-step]);
            }
        for (int i=0;i<w;++i) sum[i]=gate(2,original[i],i?g[i-1]:carry);
        return {sum,g};
    }
    std::pair<std::vector<int>,int> compress(std::vector<std::vector<int>> columns) {
        const int width=columns.size(); std::vector<std::vector<int>> summands;
        std::vector<int> packed;
        auto any=[&](){return std::any_of(columns.begin(),columns.end(),[](const auto& c){return !c.empty();});};
        while (any()) {
            std::vector<int> word(width);
            for (int i=0;i<width;++i) if (!columns[i].empty()) {
                word[i]=columns[i].front(); columns[i].erase(columns[i].begin());
            }
            summands.push_back(word);
            while (true) {
                int maximum=0,position=0;
                for (int i=0;i<width;++i) if (int(columns[i].size())>=maximum) {
                    maximum=columns[i].size(); position=i;
                }
                if (!maximum||position>4) break;
                int required=0;
                for (int i=0;i<=position;++i) if (int(columns[i].size())==maximum) required+=1<<i;
                if (required>std::max(int(summands.size())-1,0)-int(packed.size())) break;
                for (int i=0;i<=position;++i) if (int(columns[i].size())==maximum) {
                    packed.insert(packed.end(),1<<i,columns[i].front()); columns[i].erase(columns[i].begin());
                }
            }
        }
        int rounds=0;
        auto pop=[&](){if(packed.empty())return 0;int n=packed.back();packed.pop_back();return n;};
        while (summands.size()>2) {
            std::vector<std::vector<int>> following;
            for (unsigned i=0;i<summands.size();i+=3) {
                if (i+2>=summands.size()) {
                    for (unsigned j=i;j<summands.size();++j) following.push_back(summands[j]);
                    break;
                }
                std::vector<int> sums(width),carries(width);
                for (int j=0;j<width;++j) {
                    int a=summands[i][j],b=summands[i+1][j],c=summands[i+2][j],p=gate(2,a,b);
                    sums[j]=gate(2,p,c); carries[j]=gate(1,gate(0,a,b),gate(0,p,c));
                }
                carries.pop_back(); carries.insert(carries.begin(),pop());
                following.push_back(sums); following.push_back(carries);
            }
            summands=std::move(following); ++rounds;
        }
        std::vector<int> outputs(width);
        if (summands.size()==1) outputs=summands[0];
        else if (summands.size()==2) outputs=prefix(summands[0],summands[1],pop()).first;
        if (!packed.empty()) throw std::runtime_error("unconsumed compressor carry");
        return {outputs,rounds};
    }
    F work(const std::vector<int>& outputs) const {
        std::set<int> live;auto pending=outputs;std::vector<int> loads(nodes.size());
        while(!pending.empty()) {int n=pending.back();pending.pop_back();if(n<2)continue;++loads[n];
            if(nodes[n].kind<0||!live.insert(n).second)continue;
            pending.push_back(nodes[n].a);if(nodes[n].kind!=3)pending.push_back(nodes[n].b);
        }
        std::vector<int> roots,internal;
        for(unsigned n=2;n<nodes.size();++n)if(loads[n]) {
            if(nodes[n].kind<0)roots.push_back(loads[n]);else if(live.count(n))internal.push_back(loads[n]);
        }
        F result;for(const auto& [name,values]:{std::pair<std::string,std::vector<int>>{"root",roots},{"internal",internal}})
            for(const auto& [k,v]:moments(values))result[name+"_"+k]=v;
        return result;
    }
};
double price(const O& entry,const F& f) {
 double value=centeredResponse(object(entry,"function"),f);
 if(auto ref=entry.getString("reference"))value*=f.at(ref->str());
 if(!std::isfinite(value)||value<=0)throw std::runtime_error("nonpositive load-pressure price");return value;
}
}
F semanticFanoutOperatorFeatures(const O& query) {
    F f=loadPressureOperatorFeatures(query);const auto& s=object(object(query,"local_context"),"producer");Circuit c;
    int w=integer(s,"arithmetic_width"),lo=integer(s,"result_lo"),bits=integer(s,"value_width");auto op=string(s,"operation"),status=string(s,"status"),ctrl=string(s,"carry");
    auto mask=[&](const std::string& name){auto raw=string(s,name.c_str());llvm::StringRef value(raw);if(value.starts_with("0x")||value.starts_with("0X"))value=value.drop_front(2);return llvm::APInt(8193,value,16);};
    auto operand=[&](const std::string& name){int width=integer(s,(name+"_width").c_str());auto zero=mask(name+"_zero_hex"),one=mask(name+"_one_hex");std::vector<int> out;
        for(int i=0;i<w;++i){bool needed=op=="mux"?(i>=lo&&i<lo+bits):i<(status=="none"?lo+bits:w);out.push_back(i>=width||zero[i]?0:one[i]?1:needed?c.leaf():0);}return out;};
    auto a=operand("lhs"),b=operand("rhs");int control=ctrl=="dynamic"?c.leaf():int(ctrl=="one"),flag=0;std::vector<int> native;
    if(op=="add"||op=="adc"||op=="sub"||op=="sbb") {
        bool subtract=op=="sub"||op=="sbb";if(subtract){for(auto& bit:b)bit=c.gate(3,bit);control=c.gate(3,control);}
        auto sum=c.prefix(a,b,control);native=sum.first;flag=subtract?c.gate(3,sum.second.back()):sum.second.back();
    } else if(op=="mux") for(int i=0;i<w;++i)native.push_back(c.gate(1,c.gate(0,c.gate(3,control),a[i]),c.gate(0,control,b[i])));
    else throw std::runtime_error("flow-structure operator scope");
    std::vector<int> outputs(native.begin()+lo,native.begin()+lo+bits);if(status!="none")outputs.push_back(flag);F work=c.work(outputs);
    for(const auto& [k,v]:work)f["fanout_"+k]=v;
    f["fanout_repair_pressure"]=work.at("internal_log2_repair_excess")*f.at("clock_pressure");
    f["fanout_root_T"]=work.at("root_log2_concentration")*f.at("log2_target");return f;
}
O semanticFanoutReductionFeatures(const O& query) {
    O result=loadPressureReductionFeatures(query);int width=integer(query,"output_bits");auto terms=query.getArray("terms");
    auto graph=[&](bool serial){Circuit c;std::vector<std::vector<int>> vectors;
        for(const auto& raw:*terms){auto term=raw.getAsArray();if(!term)throw std::runtime_error("term required");std::vector<int> v(width);for(const auto& b:*term){auto t=b.getAsArray();v.at(int(*(*t)[2].getAsInteger()))=c.leaf();}vectors.push_back(v);}
        int rounds=0;std::vector<int> outputs;
        if(serial){outputs=vectors.at(0);for(unsigned i=1;i<vectors.size();++i)outputs=c.prefix(outputs,vectors[i]).first;}
        else {std::vector<std::vector<int>> columns(width);for(int col=0;col<width;++col)for(const auto& v:vectors)if(v[col])columns[col].push_back(v[col]);auto p=c.compress(columns);outputs=std::move(p.first);rounds=p.second;}
        return std::make_pair(c.work(outputs),rounds);};
    F extra;for(bool serial:{false,true})for(const auto& [k,v]:graph(serial).first)extra[std::string("fanout_")+(serial?"serial_":"fused_")+k]=v;
    extra["fanout_repair_pressure"]=extra.at("fanout_fused_internal_log2_repair_excess")**object(result,"intensive").getNumber("clock_pressure");
    extra["fanout_serial_concentration_gap"]=extra.at("fanout_serial_internal_log2_concentration")-extra.at("fanout_fused_internal_log2_concentration");
    for(const char* name:{"intensive","area","delay"})for(const auto& [k,v]:extra)(*result.getObject(name))[k]=v;
    return result;
}
A semanticFanoutStateFeatures(const O& query,const O& context) {
    A controls=loadPressureStateFeatures(query,context);std::map<int,int> roots;std::map<std::vector<int>,int> pairs;int sinks=0;
    for(const auto& raw:*query.getArray("input_sink_inputs")) {std::vector<int> key;for(const auto& value:*raw.getAsArray()){int r=int(*value.getAsInteger());++roots[r];key.push_back(r);}std::sort(key.begin(),key.end());++pairs[key];++sinks;}
    std::vector<int> degrees;for(const auto& [_,degree]:roots)degrees.push_back(degree);F extra;
    for(const auto& [k,v]:moments(degrees))extra["input_"+k]=v;
    extra["input_pair_reuse_fraction"]=(sinks-double(pairs.size()))/std::max(1.,double(sinks));
    for(auto& raw:controls){auto& q=*raw.getAsObject();auto& f=*q.getObject("features");double native=*f.getNumber("branch_native"),reset=*f.getNumber("reset_has_one"),logt=*f.getNumber("log2_target");
        for(const auto& [k,v]:extra)f[k]=v;
        for(const char* name:{"input_log2_concentration","input_pair_reuse_fraction","input_log2_repair_excess"}){f[std::string("native_")+name]=native*extra.at(name);f[std::string("reset_")+name]=reset*extra.at(name);}
        f["input_concentration_log_T"]=extra.at("input_log2_concentration")*logt;f["native_input_concentration_log_T"]=native*extra.at("input_log2_concentration")*logt;
    }return controls;
}
O evaluateSemanticFanout(const O& model,const std::string& kind,const O& query,bool diagnostic) {
 try {
  if(!diagnostic||model.getString("version")!="W345-semantic-fanout-functions-v8"||model.getString("feature_version")!="semantic-fanout-prospective-features-v8")throw std::runtime_error("load-pressure successor requires explicit diagnostics and matching identity");
  const auto& models=object(model,"models");
  if(kind=="S") {
   const auto& m=object(models,"S");const auto& context=object(model,"context_function");
   if(!(context==object(object(object(m,"native_enable"),"functions"),"R")))throw std::runtime_error("enable price/context dependency differs");
   auto controls=semanticFanoutStateFeatures(query,context);const auto& domains=object(model,"state_support");
   if(domains.size()!=4||m.size()!=4)throw std::runtime_error("incomplete state classes/support");
   for(const char* name:{"native_valid","native_enable","input_valid","external_valid"})
    if(!domains.getObject(name)||!m.getObject(name))throw std::runtime_error("incomplete state classes/support");
   O paths;double T=*query.getNumber("target_ps"),log=std::log2(T/500.);
   for(const auto& [name,raw]:m){const auto& entry=*raw.getAsObject();auto coeff=object(entry,"reserve_function").getArray("coefficients");if(!coeff||coeff->size()!=2)throw std::runtime_error("reserve dimensions");double guard=number((*coeff)[0])+number((*coeff)[1])*log/(1.+log);if(!std::isfinite(guard)||guard<0)throw std::runtime_error("invalid reserve");
    A prices;double maximum=0;
    for(const auto& rawq:controls){const auto& q=*rawq.getAsObject();if(q.getString("path_class")!=name.str())continue;auto f=features(object(q,"features"));supported(object(domains,name.str().c_str()),f,T);long double value=0;
     for(const auto& [_,fn]:object(entry,"functions"))value+=centeredResponse(*fn.getAsObject(),f);
     if(!std::isfinite(double(value))||value<=0)throw std::runtime_error("nonpositive complete state response");maximum=std::max(maximum,double(value));prices.push_back(O{{"stage",integer(q,"stage")},{"control_source",string(q,"control_source")},{"required_ps",double(value)},{"guarded_required_ps",double(value)+guard}});
    }
    if(prices.empty())throw std::runtime_error("missing control source");paths[name]=O{{"required_ps",maximum},{"guarded_required_ps",maximum+guard},{"sources",std::move(prices)}};
   }
   return O{{"status","diagnostic_estimate"},{"paths",std::move(paths)},{"actual_arithmetic_S_qualified",false},{"price_qualification_granted",false}};
  }
  auto gate=evaluateFlowStructure(object(model,"inherited_flow"),kind,query,true);if(gate.getString("status")=="cost_uncovered")return gate;
  if(kind=="operator") {
   auto f=semanticFanoutOperatorFeatures(query);auto op=string(object(object(query,"local_context"),"producer"),"operation");double value=price(object(models,op=="mux"?"Mux":"Add"),f);
   if(op=="mux")return O{{"status","diagnostic_delay_only"},{"operator_delay_ps",value},{"qualified_for_public_D",false}};
   auto delay=gate.getNumber("operator_delay_ps");if(!delay)throw std::runtime_error("missing retained delay");return O{{"status","diagnostic_estimate"},{"comb_area_um2",value},{"operator_delay_ps",*delay},{"qualified_for_public_D",false}};
  }
  if(kind=="G") {
   auto geometry=semanticFanoutReductionFeatures(query);auto f=features(object(geometry,"intensive"));const auto& m=object(models,"G");return O{{"status","diagnostic_estimate"},{"comb_area_um2",price(object(m,"area"),f)},{"operator_delay_ps",price(object(m,"delay"),f)},{"qualified_for_public_D",false}};
  }
  throw std::runtime_error("unknown load-pressure family");
 }catch(const std::exception& e){return O{{"status","cost_uncovered"},{"reason",std::string(e.what())}};}
}
}
