#include "backend/arithmetic_load_pressure.hpp"
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
F carryContext(const std::vector<int>& columns) {
 int active=0,tails=0,gaps=0,reach=0,longest=0;
 for(int h:columns){active+=h>0;gaps+=h==0;if(h>=2)++reach;else if(h==1&&reach){++reach;++tails;}else reach=0;longest=std::max(longest,reach);}
 return {{"carry_tail_fraction",tails/std::max(1.,double(active))},{"log2_carry_tail_bits",std::log2(tails+1.)},
 {"variable_gap_fraction",gaps/std::max(1.,double(columns.size()))},{"carry_reach_fraction",longest/std::max(1.,double(columns.size()))}};
}
double price(const O& entry,const F& f) {
 double value=centeredResponse(object(entry,"function"),f);
 if(auto ref=entry.getString("reference"))value*=f.at(ref->str());
 if(!std::isfinite(value)||value<=0)throw std::runtime_error("nonpositive load-pressure price");return value;
}
}
F loadPressureOperatorFeatures(const O& query) {
 F f=rebufferContextOperatorFeatures(query);const auto& s=object(object(query,"local_context"),"producer");
 int width=integer(s,"arithmetic_width"),lo=integer(s,"result_lo"),bits=integer(s,"value_width");
 auto op=string(s,"operation"),status=string(s,"status");std::vector<int> columns(width);
 for(const char* name:{"lhs","rhs"}) {
  int input=integer(s,(std::string(name)+"_width").c_str());
  auto mask=[&](const std::string& field){auto raw=string(s,field.c_str());llvm::StringRef v(raw);if(v.starts_with("0x")||v.starts_with("0X"))v=v.drop_front(2);return llvm::APInt(8193,v,16);};
  auto zero=mask(std::string(name)+"_zero_hex"),one=mask(std::string(name)+"_one_hex");
  for(int i=0;i<width;++i){bool needed=op=="mux"?(i>=lo&&i<lo+bits):i<(status=="none"?lo+bits:width);columns[i]+=i<input&&!zero[i]&&!one[i]&&needed;}
 }
 auto extra=carryContext(columns);for(const auto& [k,v]:extra)f[k]=v;
 f["cover_to_ripple_logratio"]=std::log2(std::max(1e-9,f.at("cover_area"))/std::max(1e-9,f.at("ripple_cover_area")));
 f["carry_tail_pressure"]=f.at("carry_tail_fraction")*f.at("clock_pressure");return f;
}
O loadPressureReductionFeatures(const O& query) {
 O result=rebufferContextReductionFeatures(query);std::vector<int> columns(integer(query,"output_bits"));
 for(const auto& term:*query.getArray("terms"))for(const auto& bit:*term.getAsArray())++columns.at(*(*bit.getAsArray())[2].getAsInteger());
 auto z=features(object(result,"intensive")),extra=carryContext(columns);
 extra["cover_to_ripple_logratio"]=std::log2(std::max(1e-9,z.at("cover_fused_area"))/std::max(1e-9,z.at("ripple_cover_fused_area")));
 extra["log2_serial_area_inflation"]=std::log2((z.at("cover_serial_area")+1.)/(z.at("cover_fused_area")+1.));
 extra["carry_tail_pressure"]=extra.at("carry_tail_fraction")*z.at("clock_pressure");
 for(const char* side:{"intensive","area","delay"})for(const auto& [k,v]:extra)(*result.getObject(side))[k]=v;return result;
}
A loadPressureStateFeatures(const O& query,const O& context) {
 A controls=rebufferContextStateFeatures(query);std::map<int,double> enable;
 for(const auto& raw:controls){const auto& q=*raw.getAsObject();if(q.getString("path_class")!="native_enable")continue;
  double value=centeredResponse(context,features(object(q,"features")));if(!std::isfinite(value)||value<=0)throw std::runtime_error("nonpositive predicted enable context");enable[integer(q,"stage")]=value;
 }
 if(enable.empty())throw std::runtime_error("missing native enable context");double T=*query.getNumber("target_ps");
 for(auto& raw:controls){auto& q=*raw.getAsObject();auto f=features(object(q,"features"));double value=enable.at(std::min(integer(q,"stage"),enable.rbegin()->first)),pressure=std::log2(1.+value/T),relaxation=(.25*std::max(0.,T-value)+.01*value)/T,native=f.at("branch_native"),reset=f.at("reset_has_one");
  F extra={{"enable_context_R_ps",value},{"predicted_enable_pressure",pressure},{"native_enable_pressure",native*pressure},{"reset_enable_pressure",reset*pressure},
   {"native_reset_enable_pressure",native*reset*pressure},{"predicted_enable_relaxation",relaxation},{"native_enable_relaxation",native*relaxation}};
  for(const auto& [k,v]:extra)(*q.getObject("features"))[k]=v;
 }return controls;
}
O evaluateLoadPressure(const O& model,const std::string& kind,const O& query,bool diagnostic) {
 try {
  if(!diagnostic||model.getString("version")!="W345-load-pressure-functions-v7"||model.getString("feature_version")!="load-pressure-prospective-features-v7")throw std::runtime_error("load-pressure successor requires explicit diagnostics and matching identity");
  const auto& models=object(model,"models");
  if(kind=="S") {
   const auto& m=object(models,"S");const auto& context=object(model,"context_function");
   if(!(context==object(object(object(m,"native_enable"),"functions"),"R")))throw std::runtime_error("enable price/context dependency differs");
   auto controls=loadPressureStateFeatures(query,context);const auto& domains=object(model,"state_support");
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
   auto f=loadPressureOperatorFeatures(query);auto op=string(object(object(query,"local_context"),"producer"),"operation");double value=price(object(models,op=="mux"?"Mux":"Add"),f);
   if(op=="mux")return O{{"status","diagnostic_delay_only"},{"operator_delay_ps",value},{"qualified_for_public_D",false}};
   auto delay=gate.getNumber("operator_delay_ps");if(!delay)throw std::runtime_error("missing retained delay");return O{{"status","diagnostic_estimate"},{"comb_area_um2",value},{"operator_delay_ps",*delay},{"qualified_for_public_D",false}};
  }
  if(kind=="G") {
   auto geometry=loadPressureReductionFeatures(query);auto f=features(object(geometry,"intensive"));const auto& m=object(models,"G");return O{{"status","diagnostic_estimate"},{"comb_area_um2",price(object(m,"area"),f)},{"operator_delay_ps",price(object(m,"delay"),f)},{"qualified_for_public_D",false}};
  }
  throw std::runtime_error("unknown load-pressure family");
 }catch(const std::exception& e){return O{{"status","cost_uncovered"},{"reason",std::string(e.what())}};}
}
}
