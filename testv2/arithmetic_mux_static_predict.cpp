#include "backend/arithmetic_mux_context.hpp"
#include <llvm/Support/JSON.h>
#include <llvm/Support/FormatVariadic.h>
#include <fstream>
#include <iostream>
#include <cmath>
#include <limits>
using namespace pred::beir;
using Object=llvm::json::Object;
using Features=std::map<std::string,double>;
static void bad(const char* s){throw std::invalid_argument(s);}
static double number(const Object& o,const char* k){auto x=o.getNumber(k);if(!x||!std::isfinite(*x))bad("invalid numeric Mux predictor");return *x;}
static const Object& object(const Object& o,const char* k){auto x=o.getObject(k);if(!x)bad("missing Mux model object");return *x;}
static std::string text(const Object& o,const char* k){auto x=o.getString(k);if(!x)bad("missing Mux schema");return x->str();}
static double numeric(const Object& p,const Features& f){auto names=p.getArray("features");if(!names)bad("missing predictor features");std::vector<double>x;std::set<std::string>seen;
    for(const auto& v:*names){auto s=v.getAsString();if(!s||!f.count(s->str())||!seen.insert(s->str()).second)bad("unknown predictor feature");x.push_back(f.at(s->str()));}
    double result=0;auto kind=text(p,"kind");if(kind=="linear"){auto c=p.getArray("coefficients");if(!c||c->size()!=x.size())bad("coefficient mismatch");result=number(p,"intercept");
        for(std::size_t i=0;i<x.size();++i){auto v=(*c)[i].getAsNumber();if(!v||!std::isfinite(*v))bad("bad coefficient");result+=*v*x[i];}
    }else if(kind=="tree"){auto nodes=p.getArray("nodes");if(!nodes)bad("missing tree");std::set<std::int64_t>walk;std::int64_t at=0;
        for(;;){if(at<0||std::size_t(at)>=nodes->size()||!walk.insert(at).second)bad("invalid/cyclic tree");auto n=(*nodes)[at].getAsObject();if(!n)bad("invalid tree node");
            if(n->get("value")){result=number(*n,"value");break;}auto i=n->getInteger("feature");if(!i||*i<0||std::size_t(*i)>=x.size())bad("invalid tree feature");
            auto child=n->getInteger(x[*i]<=number(*n,"threshold")?"left":"right");if(!child)bad("invalid tree child");at=*child;}
    }else bad("unknown predictor kind");if(!std::isfinite(result))bad("nonfinite predictor result");return std::max(0.,result);
}
static double predict(const Object& model,const Features& f,int w,int period,bool delay){auto kind=text(model,"kind");const Object* entry=&model;
    if(kind=="legacy12")return predict(object(model,"model"),f,w,period,delay);
    auto key=std::to_string(w)+"/0/"+std::to_string(w);
    if(kind=="exact_width_target_slices")key+="/"+std::to_string(period);
    if(kind=="exact_width_slices"||kind=="exact_width_target_slices"){entry=object(model,"slices").getObject(key);if(!entry)bad("uncovered exact width/target slice");}
    auto value=numeric(object(*entry,"predictor"),f);if(delay){auto scale=entry->getNumber("delay_scale");auto margin=entry->getNumber("delay_margin_ps");
        double s=scale?*scale:1.,m=margin?*margin:0.;if(!std::isfinite(s)||!std::isfinite(m)||s<1||m<0)bad("invalid Mux reserve");value=value*s+m;}return value;
}
static NodeId add(Program& p,int w,OperationKind kind,std::vector<Operand> args={},OpCode op=OpCode::None){Signal s;s.id=p.signals.size();s.name="v"+std::to_string(s.id);s.type.width=w;Operation d;d.kind=kind;d.op=op;d.type=s.type;d.operands=std::move(args);s.driver=d;p.signals.push_back(s);return s.id;}
static Operand ref(const Program& p,NodeId id){Operand a;a.node=id;a.type=p.signals[id].type;return a;}
static NodeId input(Program& p,int w){Operand a;a.kind=OperandKind::Port;a.type.width=w;a.text="p"+std::to_string(p.signals.size());auto id=add(p,w,OperationKind::PortRead,{a});p.signals[id].port_name=a.text;
    Port b;b.name=a.text;b.direction=PortDirection::Input;b.type.width=w;b.element_nodes={id};p.ports.push_back(b);p.inputs.push_back(b.name);return id;}
static Operand literal(int w,std::string kind){Operand a;a.kind=OperandKind::Literal;a.type.width=w;a.constant.width=w;a.constant.limbs={kind=="ones"?UINT64_MAX:kind=="one"?1u:0u};return a;}
static int integer(const char* text){std::size_t n=0;auto value=std::stoi(text,&n);if(n!=std::string(text).size())bad("noncanonical integer query");return value;}
int main(int argc,char**argv){
    if(argc<6)return 2;try{int w=integer(argv[2]),banks=integer(argv[3]),period=integer(argv[5]);bool candidate=argc>6&&std::string(argv[6])=="--candidate";
        std::ifstream file(argv[1]);std::string bytes{std::istreambuf_iterator<char>(file),{}};auto parsed=llvm::json::parse(bytes);if(!parsed)bad("invalid Mux JSON");auto root=parsed->getAsObject();if(!root)bad("invalid Mux root");
        if(text(*root,"mux_feature_schema")!=arithmetic::kMuxStaticFeatureSchema||text(*root,"target_platform")!="asap7"||number(*root,"ABC_ps")!=1500)bad("Mux schema/platform mismatch");
        const auto& model=object(object(object(*root,"stages"),"grt"),"operations");const auto& mux=object(model,"mux");
        if(!candidate&&(text(*root,"quality_status")!="passed"||text(mux,"quality_status")!="passed"))bad("Mux model quality not qualified");
        if(!candidate)bad("qualified deployment requires profile-bound production adapter");
        std::string shape=argv[4];std::vector<std::string>kinds;if(shape=="mixed")kinds={"mux","mux_zero","mux_one","mux_ones","eq_zero","or"};else {int n=integer(argv[4]);if(n<2||n>4)bad("unqualified parallel count");kinds.assign(n,"mux");}
        Program p;p.function_name="MuxNativeNumericalQuery";std::vector<NodeId>members;std::vector<arithmetic::MuxStaticCapture>captures;
        for(auto kind:kinds){auto a=input(p,w);NodeId b=0,c=0,id=0;if(kind=="mux"||kind=="or")b=input(p,w);if(kind.rfind("mux",0)==0)c=input(p,1);
            if(kind=="eq_zero")id=add(p,1,OperationKind::Unary,{ref(p,a)},OpCode::LogicNot);else if(kind=="or")id=add(p,w,OperationKind::Binary,{ref(p,a),ref(p,b)},OpCode::BitOr);
            else id=add(p,w,OperationKind::Ite,{ref(p,c),ref(p,a),kind=="mux"?ref(p,b):literal(w,kind.substr(4))});members.push_back(id);captures.push_back({id,unsigned(banks)});}
        auto f=arithmetic::muxStaticContext(p,members,captures,members[0]);
        f.insert({{"lhs_width",double(w)},{"rhs_width",0},{"result_width",double(w)},{"log_width",std::log2(std::max(w,1))},{"width_product",double(w)},
            {"fanout",double(banks)},{"lhs_zero_bits",0},{"rhs_zero_bits",0},{"abc_target_ps",1500},{"sdc_period_ps",double(period)},
            {"width_result_product",double(w)*w},{"retained_product_terms",0}});
        auto domain=mux.getArray("joint_domain");if(!domain)bad("missing joint Mux domain");bool covered=false;
        for(const auto& v:*domain){auto c=v.getAsObject();if(c)covered|=number(*c,"lhs_width")==w&&number(*c,"rhs_width")==0&&number(*c,"result_width")==w&&number(*c,"sdc_period_ps")==period&&number(*c,"abc_target_ps")==1500;}
        if(!covered)bad("outside Mux exact width/target domain");
        llvm::json::Object fs;for(const auto& [name,value]:f)fs[name]=value;
        llvm::json::Object result{{"covered",true},{"candidate_numerical_mode",true},{"PPA_calls",0},{"features",std::move(fs)},
            {"delay_ps",predict(object(mux,"delay"),f,w,period,true)},{"comb_area_um2",predict(object(mux,"area"),f,w,period,false)}};
        std::cout<<llvm::formatv("{0}",llvm::json::Value(std::move(result))).str()<<"\n";
    }catch(const std::exception& e){std::cout<<llvm::formatv("{0}",llvm::json::Value(llvm::json::Object{{"covered",false},{"reason",e.what()}})).str()<<"\n";}
}
