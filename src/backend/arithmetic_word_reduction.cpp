#include "backend/arithmetic_word_reduction.hpp"
#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <tuple>
#include <vector>
namespace pred::beir::arithmetic {
namespace {
using O = llvm::json::Object;
struct Node { int kind=0,a=0,b=0; };
struct Graph {
    std::vector<Node> nodes{Node{},Node{}};
    std::map<std::tuple<int,int,int>,int> intern;
    int leaf() { nodes.push_back({}); return int(nodes.size())-1; }
    int gate(int kind,int a,int b) {
        if(kind==1) { if(!a||!b)return 0; if(a==1)return b; if(b==1||a==b)return a; }
        if(kind==2) { if(!a)return b; if(!b||a==b)return a; }
        if(kind==3) { if(!a)return b; if(!b)return a; if(a==b)return 0; }
        if(a>b)std::swap(a,b); auto key=std::make_tuple(kind,a,b);
        auto it=intern.find(key); if(it!=intern.end())return it->second;
        int id=int(nodes.size());nodes.push_back({kind,a,b});intern[key]=id;return id;
    }
};
double number(const llvm::json::Value& v) {
    auto n=v.getAsNumber();if(!n||!std::isfinite(*n))throw std::runtime_error("finite word-G number required");return *n;
}
double price(const O& model,const std::map<std::string,double>& f) {
    auto names=model.getArray("features"),c=model.getArray("coefficients");
    if(!names||!c||names->size()!=c->size())throw std::runtime_error("word-G coefficient dimensions");
    double y=model.getNumber("intercept").value_or(0.);std::set<std::string> seen;
    for(unsigned i=0;i<names->size();++i) {
        auto name=(*names)[i].getAsString();if(!name||!f.count(name->str())||!seen.insert(name->str()).second)throw std::runtime_error("word-G feature missing");
        y+=f.at(name->str())*number((*c)[i]);
    }
    if(!std::isfinite(y)||y<=0)throw std::runtime_error("nonpositive word-G price");return y;
}
std::string supported(const O& s,const std::map<std::string,double>& f,double T) {
    if(s.getString("version")!="numerical-structural-support-v2-distinct-geometry")throw std::runtime_error("word-G support identity");
    auto interval=s.getArray("target_interval_ps"),names=s.getArray("features"),bounds=s.getArray("bounds"),scales=s.getArray("scales"),anchors=s.getArray("anchors");
    if(!interval||interval->size()!=2||!names||!bounds||!scales||!anchors||names->size()!=bounds->size()||names->size()!=scales->size())throw std::runtime_error("word-G support dimensions");
    if(T<number((*interval)[0])||T>number((*interval)[1]))return "outside continuous target interval";
    std::vector<double> q;
    for(unsigned i=0;i<names->size();++i) {
        auto name=(*names)[i].getAsString();auto range=(*bounds)[i].getAsArray();
        if(!name||!range||range->size()!=2||!f.count(name->str()))throw std::runtime_error("word-G support feature");
        double x=f.at(name->str());if(x<number((*range)[0])-1e-9||x>number((*range)[1])+1e-9)return "outside structural range "+name->str();q.push_back(x);
    }
    std::set<std::vector<double>> distinct;std::vector<double> distances;
    for(const auto& raw:*anchors) {
        auto a=raw.getAsArray();if(!a||a->size()!=q.size())throw std::runtime_error("word-G anchor dimensions");
        double sum=0;std::vector<double> row;
        for(unsigned i=0;i<q.size();++i) {double scale=number((*scales)[i]),x=number((*a)[i]);if(scale<=0)throw std::runtime_error("word-G support scale");row.push_back(x);sum+=std::pow((q[i]-x)/scale,2);}
        if(!distinct.insert(row).second)throw std::runtime_error("duplicate word-G anchor");distances.push_back(std::sqrt(sum));
    }
    auto k=s.getInteger("k");auto radius=s.getNumber("radius");
    if(!k||*k<1||std::size_t(*k)>distances.size()||!radius)throw std::runtime_error("word-G neighborhood");
    std::sort(distances.begin(),distances.end());return distances[*k-1]>*radius+1e-9?"outside structural neighborhood":"";
}
}
static WideReductionFeatures wordFeatures(const O& q,double target, std::map<std::string,double>* intensive) {
    auto f=wideReductionFeatures(q,target);int width=int(*q.getInteger("output_bits"));Graph g;
    std::vector<std::vector<int>> columns(width);
    for(const auto& term:*q.getArray("terms"))for(const auto& bit:*term.getAsArray())columns[int(*(*bit.getAsArray())[2].getAsInteger())].push_back(0);
    for(auto& col:columns)for(auto& bit:col)bit=g.leaf();
    std::vector<std::vector<int>> words;std::vector<int> packed;int rounds=0;
    while(std::any_of(columns.begin(),columns.end(),[](const auto& a){return !a.empty();})) {
        std::vector<int> word;
        for(auto& col:columns) {word.push_back(col.empty()?0:col.front());if(!col.empty())col.erase(col.begin());}
        words.push_back(std::move(word));
        while(true) {
            int maximum=0,position=0;
            for(int i=0;i<width;++i)if(maximum<=int(columns[i].size())) {maximum=columns[i].size();position=i;}
            if(!maximum||position>4)break;int required=0;
            for(int i=0;i<=position;++i)if(int(columns[i].size())==maximum)required+=1<<i;
            if(required>std::max(int(words.size())-1,0)-int(packed.size()))break;
            for(int i=0;i<=position;++i)if(int(columns[i].size())==maximum) {int b=columns[i].front();columns[i].erase(columns[i].begin());for(int n=0;n<(1<<i);++n)packed.push_back(b);}
        }
    }
    auto pop=[&](){if(packed.empty())return 0;int x=packed.back();packed.pop_back();return x;};
    while(words.size()>2) {
        std::vector<std::vector<int>> following;
        for(unsigned i=0;i<words.size();i+=3) {
            if(i+2>=words.size()) {for(;i<words.size();++i)following.push_back(words[i]);break;}
            std::vector<int> sums(width),carry(width);
            for(int b=0;b<width;++b) {int a=words[i][b],bb=words[i+1][b],c=words[i+2][b],p=g.gate(3,a,bb);sums[b]=g.gate(3,p,c);carry[b]=g.gate(2,g.gate(1,a,bb),g.gate(1,p,c));}
            carry.pop_back();carry.insert(carry.begin(),pop());following.push_back(std::move(sums));following.push_back(std::move(carry));
        }
        words=std::move(following);++rounds;
    }
    int treeBoundary=g.nodes.size();std::vector<int> outputs;
    if(words.size()==1)outputs=words[0];
    else {
        int cin=pop();std::vector<int> p(width),carry(width);
        for(int i=0;i<width;++i) {p[i]=g.gate(3,words[0][i],words[1][i]);carry[i]=g.gate(1,words[0][i],words[1][i]);}
        auto initial=p;carry[0]=g.gate(2,carry[0],g.gate(1,p[0],cin));int levels=int(std::ceil(std::log2(width)));
        for(int level=1;level<=levels;++level)for(int j=(1<<level)-1;j<width;j+=1<<level) {carry[j]=g.gate(2,carry[j],g.gate(1,p[j],carry[j-(1<<(level-1))]));p[j]=g.gate(1,p[j],p[j-(1<<(level-1))]);}
        for(int level=levels;level>0;--level)for(int j=(1<<level)+(1<<(level-1))-1;j<width;j+=1<<level) {carry[j]=g.gate(2,carry[j],g.gate(1,p[j],carry[j-(1<<(level-1))]));p[j]=g.gate(1,p[j],p[j-(1<<(level-1))]);}
        for(int i=0;i<width;++i)outputs.push_back(g.gate(3,initial[i],i?carry[i-1]:cin));
    }
    if(!packed.empty())throw std::runtime_error("unconsumed word-G carry");
    std::set<int> live;std::map<int,int> depth,loads;
    std::function<int(int)> visit=[&](int n) {++loads[n];if(!g.nodes[n].kind)return 0;if(live.count(n))return depth[n];live.insert(n);depth[n]=std::max(visit(g.nodes[n].a),visit(g.nodes[n].b))+1;return depth[n];};
    int maximumDepth=0;for(auto n:outputs)maximumDepth=std::max(maximumDepth,visit(n));
    std::map<std::string,double> work;
    for(auto side:{"tree","merge"})for(auto kind:{"xor","and","or"})work[std::string("word_")+side+"_"+kind]=0;
    for(auto n:live) {std::string side=n<treeBoundary?"tree":"merge",kind=g.nodes[n].kind==1?"and":g.nodes[n].kind==2?"or":"xor";++work["word_"+side+"_"+kind];}
    int maxLoad=0;for(const auto& [n,v]:loads)maxLoad=std::max(maxLoad,v);
    work["word_live_logic"]=live.size();work["word_logic_depth"]=maximumDepth;work["word_tree_rounds"]=rounds;work["word_log2_max_fanout"]=std::log2(maxLoad+1.);work["word_pressure_logic"]=live.size()*1500./target;
    for(const auto& [name,value]:work) {f.area[name]=value;f.delay[name]=value;}
    if(intensive) {
        std::set<int> bundled;int full=0,half=0;
        auto at=[&](int kind,int a,int b) {if(a>b)std::swap(a,b);auto it=g.intern.find({kind,a,b});return it!=g.intern.end()&&live.count(it->second)?it->second:-1;};
        for(int carry:live) {
            auto node=g.nodes[carry];if(node.kind!=2||!g.nodes[node.a].kind||!g.nodes[node.b].kind)continue;
            for(auto pair:{std::make_pair(node.a,node.b),std::make_pair(node.b,node.a)}) {
                int ab=pair.first,pc=pair.second;if(g.nodes[ab].kind!=1||g.nodes[pc].kind!=1)continue;
                auto a=g.nodes[ab].a,b=g.nodes[ab].b;int p=at(3,a,b);
                if(p<0||(g.nodes[pc].a!=p&&g.nodes[pc].b!=p))continue;
                int c=g.nodes[pc].a==p?g.nodes[pc].b:g.nodes[pc].a,total=at(3,p,c);if(total<0)continue;
                std::set<int> members{carry,ab,pc,p,total};bool overlap=false;for(int n:members)overlap|=bundled.count(n);
                if(overlap)continue;bundled.insert(members.begin(),members.end());++full;break;
            }
        }
        for(int n:live) {
            if(bundled.count(n)||g.nodes[n].kind!=3)continue;
            int pair=at(1,g.nodes[n].a,g.nodes[n].b);
            if(pair>=0&&!bundled.count(pair)) {bundled.insert(n);bundled.insert(pair);++half;}
        }
        int ux=0,ua=0,uo=0;
        for(int n:live)if(!bundled.count(n)) {ux+=g.nodes[n].kind==3;ua+=g.nodes[n].kind==1;uo+=g.nodes[n].kind==2;}
        std::map<int,std::pair<int,int>> depths;
        std::function<std::pair<int,int>(int)> typed=[&](int n) {
            if(!g.nodes[n].kind)return std::make_pair(0,0);auto found=depths.find(n);if(found!=depths.end())return found->second;
            auto a=typed(g.nodes[n].a),b=typed(g.nodes[n].b);return depths[n]=std::make_pair(std::max(a.first,b.first)+(g.nodes[n].kind==3),std::max(a.second,b.second)+(g.nodes[n].kind!=3));
        };
        int xd=0,cd=0;for(int n:outputs){auto d=typed(n);xd=std::max(xd,d.first);cd=std::max(cd,d.second);}
        std::map<int,std::set<int>> atoms;std::set<int> visited;int raw=0,rawMax=0;
        std::function<bool(int)> conjunction=[&](int n) {
            if(!g.nodes[n].kind) {if(n>=2)atoms[n]={n};return n>=2;}
            if(visited.count(n))return atoms.count(n)>0;visited.insert(n);
            bool a=conjunction(g.nodes[n].a),b=conjunction(g.nodes[n].b);
            if(g.nodes[n].kind!=1||!a||!b)return false;
            atoms[n]=atoms[g.nodes[n].a];atoms[n].insert(atoms[g.nodes[n].b].begin(),atoms[g.nodes[n].b].end());
            ++raw;rawMax=std::max(rawMax,int(atoms[n].size()));return true;
        };for(int n:outputs)conjunction(n);
        double unpaired=ux+ua+uo,increment=f.delay.at("post_merge_increment_columns");
        double merge=increment+f.delay.at("post_merge_full_adder_columns")+f.delay.at("post_merge_half_adder_columns");
        std::map<std::string,double> extra{{"word_full_adder_cuts",full},{"word_half_adder_cuts",half},
            {"word_unpaired_xor",ux},{"word_unpaired_and",ua},{"word_unpaired_or",uo},
            {"word_max_xor_depth",xd},{"word_max_carry_depth",cd},{"word_no_compression",rounds==0},
            {"word_compressed_half_context",half*double(half)/std::max(1.,double(full+half))*(rounds!=0)},
            {"word_increment_timing",f.delay.at("log2_post_merge_carry_span")*increment/std::max(1.,merge)}};
        f.area.insert(extra.begin(),extra.end());f.delay.insert(extra.begin(),extra.end());
        double fraction=half/std::max(1.,double(full+half));
        *intensive={{"one",1.},{"log2_output_scale",std::log2(width/64.)},
            {"log2_logic_depth",std::log2(maximumDepth+1.)},{"half_fraction",fraction},
            {"full_fraction",full/std::max(1.,double(full+half))},
            {"unpaired_fraction",unpaired/std::max(1.,full+half+unpaired)},
            {"compressed_half_fraction",fraction*(rounds!=0)},{"increment_fraction",increment/std::max(1.,merge)},
            {"raw_and_fraction",raw/std::max(1.,double(live.size()))},{"log2_raw_and_inputs",std::log2(rawMax+1.)},
            {"log2_target",f.area.at("log2_target")},{"clock_pressure",f.area.at("clock_pressure")}};
    }
    return f;
}
WideReductionFeatures wordReductionFeatures(const O& q,double target) {return wordFeatures(q,target,nullptr);}
MappingReductionFeatures mappingReductionFeatures(const O& q,double target) {
    MappingReductionFeatures f;auto base=wordFeatures(q,target,&f.intensive);f.area=std::move(base.area);f.delay=std::move(base.delay);return f;
}
O evaluateWordReduction(const O& model,const O& q,bool diagnostic) {
    try {
        if(model.getString("version")!="common-word-reduction-functions-v2"||model.getString("feature_version")!="wide-reduction-features-v2-word-compression")throw std::runtime_error("word-G model identity");
        if(!diagnostic)throw std::runtime_error("word-G candidate lacks public qualification");
        if(q.getString("launch_type").value_or("register")!="register"||q.getInteger("capture_banks").value_or(1)!=1||q.getInteger("consumer_count").value_or(0)!=0)throw std::runtime_error("unqualified word-G boundary");
        auto T=q.getNumber("target_ps");auto support=model.getObject("support"),area=model.getObject("area"),delay=model.getObject("delay");
        if(!T||!support||!area||!delay)throw std::runtime_error("word-G price inputs required");
        auto f=wordReductionFeatures(q,*T);auto reason=supported(*support,f.area,*T);if(!reason.empty())throw std::runtime_error(reason);
        return O{{"status","diagnostic_estimate"},{"operator_delay_ps",price(*delay,f.delay)},{"comb_area_um2",price(*area,f.area)},{"qualified_for_public_D",false}};
    }catch(const std::exception& e) {return O{{"status","cost_uncovered"},{"reason",std::string(e.what())}};}
}
}

namespace pred::beir::arithmetic {
llvm::json::Object evaluateMappingReduction(const llvm::json::Object& model,const llvm::json::Object& q,bool diagnostic) {
    try {
        if(model.getString("version")!="common-positive-geometry-G-functions-v6"||model.getString("feature_version")!="common-positive-geometry-G-features-v6")throw std::runtime_error("mapping-G model identity");
        if(!diagnostic)throw std::runtime_error("mapping-G candidate lacks public qualification");
        if(q.getString("launch_type").value_or("register")!="register"||q.getInteger("capture_banks").value_or(1)!=1||q.getInteger("consumer_count").value_or(0)!=0)throw std::runtime_error("unqualified mapping-G boundary");
        auto T=q.getNumber("target_ps");auto support=model.getObject("support"),area=model.getObject("area"),delay=model.getObject("delay"),weights=model.getObject("reference_area_weights");
        if(!T||!support||!area||!delay||!weights)throw std::runtime_error("mapping-G price inputs required");
        auto f=mappingReductionFeatures(q,*T);auto reason=supported(*support,f.area,*T);if(!reason.empty())throw std::runtime_error(reason);
        const std::set<std::string> required{"word_full_adder_cuts","word_half_adder_cuts","word_unpaired_xor","word_unpaired_and","word_unpaired_or"};
        if(weights->size()!=required.size())throw std::runtime_error("mapping-G primitive weight dimensions");double reference=0.;
        for(const auto& [n,w]:*weights) {if(!required.count(n.str()))throw std::runtime_error("unknown G primitive weight");double x=number(w);if(x<=0)throw std::runtime_error("invalid G primitive weight");reference+=x*f.area.at(n.str());}
        auto positive=[&](const O& m) {
            if(m.getString("kind")!="positive_log_linear")throw std::runtime_error("G positive function identity");
            // The latent log response can be negative; positivity applies after exp.
            auto names=m.getArray("features"),coeff=m.getArray("coefficients");
            if(!names||!coeff||names->size()!=coeff->size())throw std::runtime_error("G positive coefficient dimensions");
            double latent=m.getNumber("intercept").value_or(0.);std::set<std::string> seen;
            for(unsigned i=0;i<names->size();++i) {auto n=(*names)[i].getAsString();if(!n||!f.intensive.count(n->str())||!seen.insert(n->str()).second)throw std::runtime_error("G positive feature missing");latent+=f.intensive.at(n->str())*number((*coeff)[i]);}
            return std::exp(latent);
        };
        double A=reference*positive(*area),d=positive(*delay);if(!std::isfinite(A)||!std::isfinite(d)||A<=0||d<=0)throw std::runtime_error("nonpositive or nonfinite mapping-G price");
        return O{{"status","diagnostic_estimate"},{"operator_delay_ps",d},{"comb_area_um2",A},{"qualified_for_public_D",false}};
    }catch(const std::exception& e){return O{{"status","cost_uncovered"},{"reason",std::string(e.what())}};}
}
}
