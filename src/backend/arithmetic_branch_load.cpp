// Private circuit/prefix/compressor snapshot from frozen v4 SHA256 426f4a2a44db07323df63b633d26e6416d7984c2d02f5a2448d611c261d049e2.
// Kept local so adding legal cuts cannot mutate the archived v4 implementation.
#include "backend/arithmetic_branch_load.hpp"
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
                    sums[j]=gate(2,p,c);
                    int ab=gate(0,a,b),pc=gate(0,p,c);
                    carries[j]=gate(1,ab,pc);
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
};
// Greedy legal single-output cuts. A shared or externally observed interior
// stays a boundary. This estimates logical work; it does not emulate ABC sizing.
const std::map<std::string,double> areas={
 {"and2",.08748},{"and3",.08748},{"and4",.10206},{"and5",.11664},
 {"or2",.08748},{"or3",.08748},{"or4",.10206},{"or5",.11664},
 {"nand2",.08748},{"nand3",.16038},{"nor2",.08748},{"nor3",.16038},
 {"xor2",.16038},{"not",.04374},{"ao21",.08748},{"oa21",.10206},
 {"aoi21",.11664},{"oai21",.11664}};
F cover(const Circuit& c,const std::vector<int>& outputs) {
 const auto& nodes=c.nodes;std::set<int> live,owned;std::map<int,std::vector<int>> users;auto pending=outputs;
 for(int n:outputs)users[n].push_back(-1);
 while(!pending.empty()) {
  int n=pending.back();pending.pop_back();
  if(nodes[n].kind<0||!live.insert(n).second)continue;
  auto [kind,a,b]=nodes[n];users[a].push_back(n);pending.push_back(a);
  if(kind!=3){users[b].push_back(n);pending.push_back(b);}
 }
 struct Cut{int root;std::string name;std::set<int> members;std::vector<int> leaves;};
 std::vector<Cut> chosen;const char* names[]={"and2","or2","xor2","not"};
 for(auto it=live.rbegin();it!=live.rend();++it) {
  int root=*it;if(owned.count(root))continue;auto [kind,a,b]=nodes[root];
  std::vector<Cut> candidates={{root,names[kind],{root},kind==3?std::vector<int>{a}:std::vector<int>{a,b}}};
  if(kind==0||kind==1) {
   std::set<int> members={root};std::vector<int> leaves={a,b};
   for(int fanin=3;fanin<=5;++fanin) {
    int child=-1;for(int n:leaves)if(live.count(n)&&!owned.count(n)&&nodes[n].kind==kind&&users[n].size()==1)child=std::max(child,n);
    if(child<0)break;leaves.erase(std::find(leaves.begin(),leaves.end(),child));members.insert(child);leaves.push_back(nodes[child].a);leaves.push_back(nodes[child].b);
    candidates.push_back({root,std::string(kind==0?"and":"or")+std::to_string(fanin),members,leaves});
   }
  }
  bool inverted=kind==3;int outer=inverted?a:root;
  if(live.count(outer)&&(!inverted||users[outer].size()==1)) {
   auto [op,u,v]=nodes[outer];
   if(op==0||op==1) {
    for(auto [inner,other]:{std::pair<int,int>{u,v},{v,u}})
     if(live.count(inner)&&!owned.count(inner)&&nodes[inner].kind==1-op&&users[inner].size()==1)
      candidates.push_back({root,std::string(op==0?"oa":"ao")+(inverted?"i21":"21"),{root,outer,inner},{nodes[inner].a,nodes[inner].b,other}});
    if(inverted)candidates.push_back({root,op==0?"nand2":"nor2",{root,outer},{u,v}});
   }
  }
  bool found=false;Cut best;std::tuple<double,int,std::string> bestkey;
  for(const auto& cut:candidates) {
   bool valid=true;long double primitive=0;
   for(int n:cut.members){if(owned.count(n))valid=false;if(n!=root)for(int user:users[n])if(!cut.members.count(user))valid=false;primitive+=areas.at(names[nodes[n].kind]);}
   if(!valid)continue;auto key=std::make_tuple(double(primitive-areas.at(cut.name)),int(cut.members.size()),cut.name);
   if(!found||key>bestkey){found=true;best=cut;bestkey=key;}
  }
  if(!found)throw std::runtime_error("no legal cover");owned.insert(best.members.begin(),best.members.end());chosen.push_back(best);
 }
 if(owned!=live)throw std::runtime_error("logical cover ownership");
 std::vector<int> depth(nodes.size());long double area=0;int wide=0,compound=0;
 for(auto it=chosen.rbegin();it!=chosen.rend();++it){int d=0;for(int n:it->leaves)d=std::max(d,depth[n]);depth[it->root]=d+1;}
 for(const auto& cut:chosen){area+=areas.at(cut.name);wide+=cut.name=="and3"||cut.name=="and4"||cut.name=="and5"||cut.name=="or3"||cut.name=="or4"||cut.name=="or5";compound+=cut.name=="ao21"||cut.name=="oa21"||cut.name=="aoi21"||cut.name=="oai21";}
 int maximum=0;for(int n:outputs)maximum=std::max(maximum,depth[n]);
 return {{"area",double(area)},{"cells",double(chosen.size())},{"depth",double(maximum)},
  {"wide_cells",double(wide)},{"compound_cells",double(compound)},{"absorbed_gates",double(live.size()-chosen.size())}};
}

F operatorCover(const O& query) {
    const auto& s=object(object(query,"local_context"),"producer");Circuit c;
    int w=integer(s,"arithmetic_width"),lo=integer(s,"result_lo"),bits=integer(s,"value_width");auto op=string(s,"operation"),status=string(s,"status"),ctrl=string(s,"carry");
    auto mask=[&](const std::string& name){auto raw=string(s,name.c_str());llvm::StringRef value(raw);if(value.starts_with("0x")||value.starts_with("0X"))value=value.drop_front(2);return llvm::APInt(8193,value,16);};
    auto operand=[&](const std::string& name){int width=integer(s,(name+"_width").c_str());auto zero=mask(name+"_zero_hex"),one=mask(name+"_one_hex");std::vector<int> out;
        for(int i=0;i<w;++i){bool needed=op=="mux"?(i>=lo&&i<lo+bits):i<(status=="none"?lo+bits:w);out.push_back(i>=width||zero[i]?0:one[i]?1:needed?c.leaf():0);}return out;};
    auto a=operand("lhs"),b=operand("rhs");int control=ctrl=="dynamic"?c.leaf():int(ctrl=="one"),flag=0;std::vector<int> native;
    if(op=="add"||op=="adc"||op=="sub"||op=="sbb") {
        bool subtract=op=="sub"||op=="sbb";if(subtract){for(auto& bit:b)bit=c.gate(3,bit);control=c.gate(3,control);}
        auto sum=c.prefix(a,b,control);native=sum.first;flag=subtract?c.gate(3,sum.second.back()):sum.second.back();
    } else if(op=="mux") for(int i=0;i<w;++i) {
        int inverted=c.gate(3,control),left=c.gate(0,inverted,a[i]),right=c.gate(0,control,b[i]);
        native.push_back(c.gate(1,left,right));
    }
    else throw std::runtime_error("flow-structure operator scope");
    std::vector<int> outputs(native.begin()+lo,native.begin()+lo+bits);if(status!="none")outputs.push_back(flag);return cover(c,outputs);
}
}
F branchLoadOperatorFeatures(const O& query) {
 F f=flowStructureOperatorFeatures(query);
 F work=operatorCover(query);
 for(const auto& [name,v]:work)f["cover_"+name]=v;
 double log=std::log2(work.at("cells")+1.),load=std::log2(1.+work.at("cells")/64.);
 f["log2_cover_work"]=log;f["squared_log_cover_work"]=log*log;
 f["cover_pressure_response"]=load*f.at("clock_pressure");f["cover_T_response"]=load*f.at("log2_target");
 f["cover_compound_fraction"]=work.at("compound_cells")/std::max(1.,work.at("cells"));f["cover_wide_fraction"]=work.at("wide_cells")/std::max(1.,work.at("cells"));
 f["work_pressure"]=f.at("log2_ks_work")*f.at("clock_pressure");f["work_log_target"]=f.at("log2_ks_work")*f.at("log2_target");
 f["selector_log_load"]=f.count("control_input_bits")?std::log2(f.at("control_input_bits")+1.):f.at("log2_ks_work");f["log_target_squared"]=f.at("log2_target")*f.at("log2_target");return f;
}
O branchLoadReductionFeatures(const O& query) {
 O result=flowStructureReductionFeatures(query);int width=integer(query,"output_bits");auto terms=query.getArray("terms");if(!terms)throw std::runtime_error("terms required");
 for(bool serial:{false,true}) {
  Circuit c;std::vector<std::vector<int>> vectors;
  for(const auto& raw:*terms){auto term=raw.getAsArray();if(!term)throw std::runtime_error("term required");std::vector<int> v(width);for(const auto& bit:*term)v.at(int(*(*bit.getAsArray())[2].getAsInteger()))=c.leaf();vectors.push_back(v);}
  std::vector<int> outputs;
  if(serial){outputs=vectors.at(0);for(unsigned i=1;i<vectors.size();++i)outputs=c.prefix(outputs,vectors[i]).first;}
  else{std::vector<std::vector<int>> columns(width);for(int col=0;col<width;++col)for(const auto& v:vectors)if(v[col])columns[col].push_back(v[col]);outputs=c.compress(columns).first;}
  auto work=cover(c,outputs);for(const char* side:{"intensive","area","delay"})for(const auto& [name,v]:work)(*result.getObject(side))[std::string(serial?"cover_serial_":"cover_fused_")+name]=v;
 }
 auto z=features(object(result,"intensive"));bool risk=z.at("initial_sum_precision")<z.at("effective_sum_precision")&&z.at("ks_compressor_rounds")==0.;
 std::set<int> previous;double overlap=0,total=0,maximum=0;
 for(const auto& raw:*terms){std::set<int> span;for(const auto& bit:*raw.getAsArray())span.insert(int(*(*bit.getAsArray())[2].getAsInteger()));int count=0;for(int col:span)count+=previous.count(col);overlap+=count;maximum=std::max(maximum,double(count));total+=span.size();previous.insert(span.begin(),span.end());}
 F extra={{"log2_cover_boundary_depth",std::log2(std::max(z.at("cover_fused_depth"),risk?z.at("cover_serial_depth"):0.)+1.)},
 {"cover_wide_fraction",z.at("cover_fused_wide_cells")/std::max(1.,z.at("cover_fused_cells"))},
 {"cover_compound_fraction",z.at("cover_fused_compound_cells")/std::max(1.,z.at("cover_fused_cells"))},
 {"word_overlap_fraction",overlap/std::max(1.,total)},{"log2_max_word_overlap",std::log2(maximum+1.)}};
 for(const char* side:{"intensive","area","delay"})for(const auto& [name,v]:extra)(*result.getObject(side))[name]=v;return result;
}
A branchLoadStateFeatures(const O& query) {
 A controls=flowStructureStateFeatures(query);double input=0;for(const auto& w:*query.getArray("input_widths"))input+=number(w);
 for(auto& raw:controls){auto& q=*raw.getAsObject();auto f=features(object(q,"features"));double native=f.at("native_launch"),sinks=f.at("enabled_data_sinks"),ones=sinks*f.at("capture_bank_reset_fraction"),zeros=sinks-ones,reset=ones>0,logt=f.at("log2_target"),capture=q.getString("path_class")=="input_valid"?input:std::exp2(f.at("log2_capture_bank_bits"))-1.;
  F extra={{"branch_native",native},{"log2_context_capture",std::log2(capture+1.)},{"log2_global_FF",std::log2(f.at("total_FF_bits")+1.)},
   {"reset_has_one",reset},{"branch_reset_one",native*reset},{"reset_load_imbalance",std::abs(zeros-ones)/std::max(1.,sinks)},
   {"branch_load",native*f.at("log2_logical_sinks")},{"branch_log_T",native*logt},{"branch_log_T2",native*logt*logt},{"log_T2",logt*logt},
   {"reset_log_load",reset*f.at("log2_logical_sinks")},{"branch_reset_log_load",native*reset*f.at("log2_logical_sinks")},
   {"reset_log_T",reset*logt},{"branch_reset_log_T",native*reset*logt},{"reset_log_T2",reset*logt*logt},{"branch_reset_log_T2",native*reset*logt*logt},
   {"log2_mean_word",std::log2(sinks/std::max(1.,f.at("enabled_word_count"))+1.)}};
  for(const auto& [name,v]:extra)(*q.getObject("features"))[name]=v;
 }
 return controls;
}
double branchPrice(const O& entry,const F& f) {
 double value=centeredResponse(object(entry,"function"),f);if(auto ref=entry.getString("reference"))value*=f.at(ref->str());
 if(!std::isfinite(value)||value<=0)throw std::runtime_error("nonpositive branch-load price");return value;
}

O evaluateBranchLoad(const O& model,const std::string& kind,const O& query,bool diagnostic) {
    try {
        if(!diagnostic||model.getString("version")!="W345-branch-load-functions-v5"||model.getString("feature_version")!="branch-load-prospective-features-v5")throw std::runtime_error("branch-load successor requires explicit diagnostics and matching identity");
        auto gate=evaluateFlowStructure(object(model,"inherited_flow"),kind,query,true);
        if(gate.getString("status")=="cost_uncovered")return gate;
        const auto& models=object(model,"models");
        if(kind=="operator") {
            auto f=branchLoadOperatorFeatures(query);auto op=string(object(object(query,"local_context"),"producer"),"operation");const char* name=op=="mux"?"Mux":"Add";auto& m=object(models,name);double value=branchPrice(m,f);
            if(op=="mux")return O{{"status","diagnostic_delay_only"},{"operator_delay_ps",value},{"qualified_for_public_D",false}};
            auto reference=m.getString("reference");if(!reference)throw std::runtime_error("Add reference required");double area=value;auto delay=gate.getNumber("operator_delay_ps");if(area<=0||!delay)throw std::runtime_error("nonpositive operator price");
            return O{{"status","diagnostic_estimate"},{"comb_area_um2",area},{"operator_delay_ps",*delay},{"qualified_for_public_D",false}};
        }
        if(kind=="G") {
            auto geometry=branchLoadReductionFeatures(query);auto f=features(object(geometry,"intensive"));auto& m=object(models,"G");double area=branchPrice(object(m,"area"),f),delay=branchPrice(object(m,"delay"),f);
            if(area<=0||delay<=0)throw std::runtime_error("nonpositive reduction price");return O{{"status","diagnostic_estimate"},{"comb_area_um2",area},{"operator_delay_ps",delay},{"qualified_for_public_D",false}};
        }
        if(kind=="S") {
            auto controls=branchLoadStateFeatures(query);auto& m=object(models,"S");O paths;double log=std::log2(*query.getNumber("target_ps")/500.);
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
