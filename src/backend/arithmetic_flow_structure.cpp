#include "backend/arithmetic_flow_structure.hpp"
#include <llvm/ADT/APInt.h>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
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
double primitiveArea(const F& f) {
    return .16038*f.at("xor") + .08748*(f.at("and")+f.at("or")) + .05832*f.at("not");
}
double cutArea(const F& f) {
    return .20412*f.at("full_cuts") + .13122*f.at("half_cuts")
        + .16038*f.at("unpaired_xor") + .08748*(f.at("unpaired_and")+f.at("unpaired_or"))
        + .05832*f.at("unpaired_not");
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
        std::set<int> live; auto pending=outputs; std::vector<int> loads(nodes.size());
        while (!pending.empty()) {
            int n=pending.back();pending.pop_back();++loads[n];
            if (n<2||nodes[n].kind<0||!live.insert(n).second) continue;
            pending.push_back(nodes[n].a);pending.push_back(nodes[n].b);
        }
        std::vector<int> depth(nodes.size()),xd(nodes.size()),cd(nodes.size());
        std::map<std::tuple<int,int,int>,int> lookup;
        for (int n:live) {
            auto [kind,a,b]=nodes[n]; depth[n]=std::max(depth[a],depth[b])+1;
            xd[n]=std::max(xd[a],xd[b])+(kind==2);cd[n]=std::max(cd[a],cd[b])+(kind!=2);
            lookup[{kind,a,b}]=n;
        }
        auto at=[&](int kind,int a,int b){if(a>b)std::swap(a,b);auto it=lookup.find({kind,a,b});return it==lookup.end()?-1:it->second;};
        std::set<int> bundled; int full=0,half=0;
        for (int carry:live) {
            auto [kind,u,v]=nodes[carry];
            if (kind!=1||nodes[u].kind<0||nodes[v].kind<0) continue;
            for (auto [ab,pc]:{std::pair<int,int>{u,v},{v,u}}) {
                if (nodes[ab].kind!=0||nodes[pc].kind!=0) continue;
                int a=nodes[ab].a,b=nodes[ab].b,p=at(2,a,b);
                if (p<0||(nodes[pc].a!=p&&nodes[pc].b!=p)) continue;
                int c=nodes[pc].a==p?nodes[pc].b:nodes[pc].a,total=at(2,p,c);
                std::set<int> members={carry,ab,pc,p,total};bool overlaps=false;
                for (int n:members) overlaps|=bundled.count(n)>0;
                if (total>=0&&!overlaps) {bundled.insert(members.begin(),members.end());++full;break;}
            }
        }
        for (int n:live) if (!bundled.count(n)&&nodes[n].kind==2) {
            int pair=at(0,nodes[n].a,nodes[n].b);
            if (pair>=0&&!bundled.count(pair)) {bundled.insert(n);bundled.insert(pair);++half;}
        }
        F result={{"and",0},{"or",0},{"xor",0},{"not",0},{"unpaired_and",0},{"unpaired_or",0},{"unpaired_xor",0},{"unpaired_not",0},
            {"work",double(live.size())},{"depth",0},{"xor_depth",0},{"carry_depth",0},{"full_cuts",double(full)},{"half_cuts",double(half)},
            {"log2_max_fanout",std::log2(*std::max_element(loads.begin(),loads.end())+1.)}};
        const char* names[]={"and","or","xor","not"};
        for (int n:live) {++result[names[nodes[n].kind]];if(!bundled.count(n))++result[std::string("unpaired_")+names[nodes[n].kind]];}
        for (int n:outputs) {result["depth"]=std::max(result["depth"],double(depth[n]));result["xor_depth"]=std::max(result["xor_depth"],double(xd[n]));result["carry_depth"]=std::max(result["carry_depth"],double(cd[n]));}
        return result;
    }
};
}

NumericalFeatures flowStructureOperatorFeatures(const O& query) {
    F f=commonMechanismOperatorFeatures(query);const auto& s=object(object(query,"local_context"),"producer");Circuit c;
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
    for(const auto& [name,value]:work)f["ks_"+name]=value;
    f["ks_primitive_area"]=primitiveArea(work);f["ks_cut_area"]=cutArea(work);f["log2_ks_work"]=std::log2(work["work"]+1.);f["log2_ks_depth"]=std::log2(work["depth"]+1.);
    f["ks_unpaired_fraction"]=(work["unpaired_xor"]+work["unpaired_and"]+work["unpaired_or"]+work["unpaired_not"])/std::max(1.,work["work"]);
    f["ks_half_fraction"]=work["half_cuts"]/std::max(1.,work["full_cuts"]+work["half_cuts"]);return f;
}

O flowStructureReductionFeatures(const O& query) {
    O result=commonMechanismReductionFeatures(query);int width=integer(query,"output_bits");auto terms=query.getArray("terms");if(!terms)throw std::runtime_error("reduction terms required");
    auto graph=[&](bool serial){Circuit c;std::vector<std::vector<int>> vectors;
        for(const auto& raw:*terms){auto term=raw.getAsArray();if(!term)throw std::runtime_error("term required");std::vector<int> v(width);for(const auto& b:*term){auto t=b.getAsArray();v.at(int(*(*t)[2].getAsInteger()))=c.leaf();}vectors.push_back(v);}
        int rounds=0;std::vector<int> outputs;
        if(serial){outputs=vectors.at(0);for(unsigned i=1;i<vectors.size();++i)outputs=c.prefix(outputs,vectors[i]).first;}
        else {std::vector<std::vector<int>> columns(width);for(int col=0;col<width;++col)for(const auto& v:vectors)if(v[col])columns[col].push_back(v[col]);auto p=c.compress(columns);outputs=std::move(p.first);rounds=p.second;}
        return std::make_pair(c.work(outputs),rounds);};
    auto [fused,rounds]=graph(false);auto serial=graph(true).first;auto intensive=result.getObject("intensive");
    for(const auto& [name,value]:fused)(*intensive)["ks_fused_"+name]=value;
    for(const auto& [name,value]:serial)(*intensive)["ks_serial_"+name]=value;
    (*intensive)["ks_fused_area"]=cutArea(fused);(*intensive)["ks_serial_area"]=cutArea(serial);
    F extra={{"log2_ks_depth",std::log2(fused["depth"]+1.)},{"log2_ks_serial_depth",std::log2(serial["depth"]+1.)},
        {"ks_serial_depth_gap",(serial["depth"]-fused["depth"])/std::max(1.,fused["depth"])},{"ks_serial_work_ratio",serial["work"]/std::max(1.,fused["work"])},
        {"ks_fused_half_fraction",fused["half_cuts"]/std::max(1.,fused["full_cuts"]+fused["half_cuts"])},
        {"ks_fused_unpaired_fraction",(fused["unpaired_xor"]+fused["unpaired_and"]+fused["unpaired_or"]+fused["unpaired_not"])/std::max(1.,fused["work"])},
        {"ks_serial_carry_depth",serial["carry_depth"]},{"log2_ks_fanout",fused["log2_max_fanout"]},{"ks_compressor_rounds",double(rounds)}};
    llvm::APInt maximum(8193,0);int first=0;
    for(unsigned i=0;i<terms->size();++i)for(const auto& raw:*(*terms)[i].getAsArray()){int col=int(*(*raw.getAsArray())[2].getAsInteger());maximum+=llvm::APInt(8193,1).shl(col);if(i<2)first=std::max(first,col+1);}
    first=std::min(width,first+1);int effective=std::min(width,int(maximum.getActiveBits()));bool risk=first<effective;
    double depth=std::max(fused["depth"],risk?serial["depth"]:0.);
    extra["initial_sum_precision"]=first;extra["effective_sum_precision"]=effective;extra["intermediate_extension_fraction"]=std::max(0.,double(effective-first))/width;
    extra["log2_boundary_logic_depth"]=std::log2(depth+1.);extra["ks_shrink_work_ratio"]=1.+double(risk)*(extra["ks_serial_work_ratio"]-1.);extra["ks_shrink_depth_gap"]=double(risk)*extra["ks_serial_depth_gap"];
    risk=risk&&rounds==0;depth=std::max(fused["depth"],risk?serial["depth"]:0.);extra["log2_sparse_boundary_depth"]=std::log2(depth+1.);extra["ks_sparse_boundary_work_ratio"]=1.+double(risk)*(extra["ks_serial_work_ratio"]-1.);
    for(const char* name:{"area","delay","intensive"})for(const auto& [k,v]:extra)(*result.getObject(name))[k]=v;
    return result;
}

A flowStructureStateFeatures(const O& structure) {
    A controls=commonMechanismStateFeatures(structure);auto stages=structure.getArray("stage_words"),roots=structure.getArray("input_widths"),resets=structure.getArray("stage_reset_one_bits");
    if(!stages||!roots||!resets||stages->empty())throw std::runtime_error("explicit local state banks required");
    double input=0;for(const auto& w:*roots)input+=number(w);std::vector<double> bits;
    for(const auto& raw:*stages){double n=0;for(const auto& w:*raw.getAsArray())n+=number(w);bits.push_back(n);}
    for(auto& raw:controls){auto& q=*raw.getAsObject();auto& f=*q.getObject("features");int stage=integer(q,"stage");auto source=string(q,"control_source");bool isInput=source=="port_iv"||source=="service_valid";int previous=std::min(std::max(stage-1,0),int(bits.size())-1);
        double launch=isInput?input:bits[previous],capture=stage<int(bits.size())?bits[stage]:integer(structure,"external_bits"),lr=isInput?integer(structure,"input_reset_one_bits"):number((*resets)[previous]),cr=stage<int(bits.size())?number((*resets)[stage]):0.;
        f["log2_source_bank_bits"]=std::log2(launch+1.);f["log2_capture_bank_bits"]=std::log2(capture+1.);f["source_bank_reset_fraction"]=lr/std::max(1.,launch);f["capture_bank_reset_fraction"]=cr/std::max(1.,capture);f["source_has_hold"]=double(isInput&&integer(structure,"service_mode")!=2);f["stage_fraction"]=stage/double(std::max(1,int(bits.size())));f["log2_state_depth"]=std::log2(bits.size()+1.);f["target_linear"]=*structure.getNumber("target_ps")/1500.;f["source_sqrt_bank_bits"]=std::sqrt(launch);f["capture_sqrt_bank_bits"]=std::sqrt(capture);
        double fraction=*f.getNumber("capture_bank_reset_fraction"),load=*f.getNumber("enabled_data_sinks"),native=*f.getNumber("native_launch");
        f["log2_reset_zero_sinks"]=std::log2(load*(1.-fraction)+1.);f["log2_reset_one_sinks"]=std::log2(load*fraction+1.);f["native_reset_one_load"]=native**f.getNumber("log2_reset_one_sinks");f["native_capture_reset_fraction"]=native*fraction;f["native_source_reset_fraction"]=native**f.getNumber("source_bank_reset_fraction");
    }
    return controls;
}

O evaluateFlowStructure(const O& model,const std::string& kind,const O& query,bool diagnostic) {
    try {
        if(!diagnostic||model.getString("version")!="W345-flow-word-context-functions-v4"||model.getString("feature_version")!="flow-structure-word-context-features-v4")throw std::runtime_error("flow-structure successor requires explicit diagnostics and matching identity");
        auto gate=evaluateCommonCentered(object(model,"inherited_centered"),kind,query,true);
        if(gate.getString("status")=="cost_uncovered")return gate;
        const auto& models=object(model,"models");
        if(kind=="operator") {
            auto f=flowStructureOperatorFeatures(query);auto op=string(object(object(query,"local_context"),"producer"),"operation");const char* name=op=="mux"?"Mux":"Add";auto& m=object(models,name);double value=centeredResponse(object(m,"function"),f);
            if(op=="mux")return O{{"status","diagnostic_delay_only"},{"operator_delay_ps",value},{"qualified_for_public_D",false}};
            auto reference=m.getString("reference");if(!reference)throw std::runtime_error("Add reference required");double area=value*f.at(reference->str());auto delay=gate.getNumber("operator_delay_ps");if(area<=0||!delay)throw std::runtime_error("nonpositive operator price");
            return O{{"status","diagnostic_estimate"},{"comb_area_um2",area},{"operator_delay_ps",*delay},{"qualified_for_public_D",false}};
        }
        if(kind=="G") {
            auto geometry=flowStructureReductionFeatures(query);auto f=features(object(geometry,"intensive"));auto& m=object(models,"G");double area=f.at("ks_fused_area")*centeredResponse(object(m,"area"),f),delay=centeredResponse(object(m,"delay"),f);
            if(area<=0||delay<=0)throw std::runtime_error("nonpositive reduction price");return O{{"status","diagnostic_estimate"},{"comb_area_um2",area},{"operator_delay_ps",delay},{"qualified_for_public_D",false}};
        }
        if(kind=="S") {
            auto controls=flowStructureStateFeatures(query);auto& m=object(models,"S");O paths;double log=std::log2(*query.getNumber("target_ps")/500.);
            for(const auto& [name,raw]:m) {auto& entry=*raw.getAsObject();auto coeff=object(entry,"reserve_function").getArray("coefficients");if(!coeff||coeff->size()!=2)throw std::runtime_error("reserve dimensions");double guard=number((*coeff)[0])+number((*coeff)[1])*log/(1.+log);if(!std::isfinite(guard)||guard<0)throw std::runtime_error("invalid reserve");
                A prices;double maximum=0;
                for(const auto& rawq:controls){auto& q=*rawq.getAsObject();if(q.getString("path_class")!=name.str())continue;auto f=features(object(q,"features"));long double value=0;for(const auto& [component,fn]:object(entry,"functions"))value+=centeredResponse(*fn.getAsObject(),f);if(!std::isfinite(double(value))||value<=0)throw std::runtime_error("nonpositive complete state response");maximum=std::max(maximum,double(value));prices.push_back(O{{"stage",integer(q,"stage")},{"control_source",string(q,"control_source")},{"required_ps",double(value)},{"guarded_required_ps",double(value)+guard}});}
                if(prices.empty())throw std::runtime_error("missing control source");paths[name]=O{{"required_ps",maximum},{"guarded_required_ps",maximum+guard},{"sources",std::move(prices)}};
            }
            return O{{"status","diagnostic_estimate"},{"paths",std::move(paths)},{"actual_arithmetic_S_qualified",false},{"price_qualification_granted",false}};
        }
        throw std::runtime_error("unknown flow-structure family");
    } catch(const std::exception& e) { return O{{"status","cost_uncovered"},{"reason",std::string(e.what())}}; }
}
}
