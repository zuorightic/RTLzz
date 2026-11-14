#include "backend/arithmetic_centered_response.hpp"
#include "backend/arithmetic_wide_alu.hpp"
#include "backend/arithmetic_operator_successor.hpp"
#include "backend/arithmetic_word_reduction.hpp"
#include "backend/arithmetic_control_successor.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <vector>
namespace pred::beir::arithmetic {
namespace {
using O = llvm::json::Object;
using A = llvm::json::Array;
double num(const llvm::json::Value& v) {
    auto n = v.getAsNumber();
    if (!n || !std::isfinite(*n)) throw std::runtime_error("finite numerical parameter required");
    return *n;
}
const O& object(const O& o, const char* n) {
    auto p = o.getObject(n);
    if (!p) throw std::runtime_error("required object missing");
    return *p;
}
NumericalFeatures features(const O& o) {
    NumericalFeatures f;
    for (const auto& [n, v] : o) f[n.str()] = num(v);
    return f;
}
O json(const NumericalFeatures& f) {
    O result;
    for (const auto& [n, v] : f) result[n] = v;
    return result;
}
NumericalFeatures carry(const std::vector<int>& heights) {
    int reach=0, longest=0, single=0, propagated=0, runs=0;
    bool previous=false;
    for (int height : heights) {
        bool overlap=height>=2;
        runs+=overlap&&!previous; previous=overlap;
        if (overlap || (height==1&&reach)) {
            ++reach; longest=std::max(longest,reach); ++propagated; single+=height==1;
        } else reach=0;
    }
    return {{"log2_carry_reach",std::log2(longest+1.)},
            {"carry_single_fraction",single/std::max(1.,double(propagated))},
            {"log2_overlap_runs",std::log2(runs+1.)}};
}
void supported(const O& s, const NumericalFeatures& f, double T) {
    if (s.getString("version")!="numerical-structural-support-v2-distinct-geometry") throw std::runtime_error("support identity");
    auto interval=s.getArray("target_interval_ps"), names=s.getArray("features"), bounds=s.getArray("bounds"), scales=s.getArray("scales"), anchors=s.getArray("anchors");
    if (!interval||interval->size()!=2||!names||!bounds||!scales||!anchors||names->size()!=bounds->size()||names->size()!=scales->size()) throw std::runtime_error("support dimensions");
    if (T<num((*interval)[0])||T>num((*interval)[1])) throw std::runtime_error("outside continuous target interval");
    std::vector<double> q;
    for (unsigned i=0;i<names->size();++i) {
        auto n=(*names)[i].getAsString(); auto range=(*bounds)[i].getAsArray();
        if (!n||!range||range->size()!=2) throw std::runtime_error("support feature");
        double v=f.at(n->str());
        if (v<num((*range)[0])-1e-9||v>num((*range)[1])+1e-9) throw std::runtime_error("outside structural range "+n->str());
        q.push_back(v);
    }
    std::vector<double> distances; std::set<std::vector<double>> distinct;
    for (const auto& raw : *anchors) {
        auto a=raw.getAsArray(); if (!a||a->size()!=q.size()) throw std::runtime_error("support anchor dimensions");
        double sum=0; std::vector<double> v;
        for (unsigned i=0;i<q.size();++i) {double x=num((*a)[i]),scale=num((*scales)[i]); if(scale<=0)throw std::runtime_error("support scale");v.push_back(x);sum+=std::pow((q[i]-x)/scale,2);}
        if (!distinct.insert(v).second) throw std::runtime_error("duplicate support anchor");
        distances.push_back(std::sqrt(sum));
    }
    auto k=s.getInteger("k"); auto radius=s.getNumber("radius");
    if (!k||*k<1||std::size_t(*k)>distances.size()||!radius) throw std::runtime_error("support neighborhood dimensions");
    std::sort(distances.begin(),distances.end());
    if (distances[*k-1]>*radius+1e-9) throw std::runtime_error("outside structural neighborhood");
}
double linear(const O& m,const NumericalFeatures& f) {
    auto names=m.getArray("features"),coeff=m.getArray("coefficients");
    if (!names||!coeff||names->size()!=coeff->size())throw std::runtime_error("linear dimensions");
    long double value=m.getNumber("intercept").value_or(0.);
    for(unsigned i=0;i<names->size();++i)value+=f.at((*names)[i].getAsString()->str())*num((*coeff)[i]);
    return std::max(0.,double(value));
}
}
double centeredResponse(const O& model,const NumericalFeatures& f) {
    if (model.getString("version")!="centered-numerical-response-v1")throw std::runtime_error("centered response identity mismatch");
    auto names=model.getArray("features"),centers=model.getArray("centers"),scales=model.getArray("scales"),coeff=model.getArray("coefficients");
    auto link=model.getString("link");
    if (!names||!centers||!scales||!coeff||!link||names->size()!=centers->size()||names->size()!=scales->size()||names->size()!=coeff->size()||(*link!="linear"&&*link!="positive_log_linear"))throw std::runtime_error("centered response dimensions or link");
    auto intercept=model.getNumber("intercept");if(!intercept||!std::isfinite(*intercept))throw std::runtime_error("centered response intercept");
    long double latent=*intercept;std::set<std::string> seen;
    for(unsigned i=0;i<names->size();++i) {
        auto n=(*names)[i].getAsString();
        if(!n||!seen.insert(n->str()).second)throw std::runtime_error("duplicate centered feature");
        double x=f.at(n->str()),center=num((*centers)[i]),scale=num((*scales)[i]),c=num((*coeff)[i]);
        if(!std::isfinite(x)||scale<=0)throw std::runtime_error("finite features and positive scales required");
        // Evaluate products as double, matching Python; accumulate accurately.
        latent+=((x-center)/scale)*c;
    }
    double result=*link=="positive_log_linear"?std::exp(double(latent)):double(latent);
    if(!std::isfinite(result))throw std::runtime_error("nonfinite centered response");return result;
}
NumericalFeatures commonMechanismOperatorFeatures(const O& q) {
    auto f=successorOperatorFeatures(q);auto& spec=object(object(q,"local_context"),"producer");
    auto g=demandedWideAluGeometry(spec);int width=int(*spec.getInteger("arithmetic_width"));std::vector<int> heights(width);
    std::set<int> a(g.lhs_bits.begin(),g.lhs_bits.end()),b(g.rhs_bits.begin(),g.rhs_bits.end());
    int overlap=0,occupied=0;
    for(int i=0;i<width;++i){heights[i]=a.count(i)+b.count(i);overlap+=heights[i]==2;occupied+=heights[i]>0;}
    auto c=carry(heights);f["alu_overlap_fraction"]=overlap/std::max(1.,double(occupied));
    f["alu_carry_reach_fraction"]=(std::pow(2.,c.at("log2_carry_reach"))-1.)/width;return f;
}
O commonMechanismReductionFeatures(const O& q) {
    auto T=q.getNumber("target_ps");if(!T)throw std::runtime_error("numerical target required");
    auto f=mappingReductionFeatures(q,*T);int width=int(*q.getInteger("output_bits"));std::vector<int> heights(width);
    for(const auto& term:*q.getArray("terms"))for(const auto& bit:*term.getAsArray())++heights.at(*(*bit.getAsArray())[2].getAsInteger());
    auto extra=carry(heights);f.area.insert(extra.begin(),extra.end());f.delay.insert(extra.begin(),extra.end());f.intensive.insert(extra.begin(),extra.end());
    return O{{"area",json(f.area)},{"delay",json(f.delay)},{"intensive",json(f.intensive)}};
}
A commonMechanismStateFeatures(const O& s) {
    auto qs=prospectiveControlFeatures(s);auto roots=s.getArray("input_widths"),stages=s.getArray("stage_words"),sinks=s.getArray("input_sink_inputs"),resets=s.getArray("stage_reset_one_bits");auto inputReset=s.getInteger("input_reset_one_bits");
    if(!roots||!stages||!sinks||!resets||resets->size()!=stages->size()||!inputReset||sinks->size()>1048576)throw std::runtime_error("prospective state reset/connections required");
    std::size_t bits=0;for(const auto& w:*roots)bits+=*w.getAsInteger();if(*inputReset<0||std::size_t(*inputReset)>bits)throw std::runtime_error("input reset polarity outside bits");
    std::vector<unsigned> counts(bits);std::vector<double> fractions;
    for(const auto& raw:*sinks){auto a=raw.getAsArray();if(!a||a->empty())throw std::runtime_error("logical sink required");std::set<unsigned> unique;
        for(const auto& bit:*a){auto b=bit.getAsInteger();if(!b||*b<0||std::size_t(*b)>=bits)throw std::runtime_error("logical sink outside roots");unique.insert(*b);}for(auto b:unique)++counts[b];}
    double loaded=0,maximum=0,logsum=0;
    for(auto n:counts){loaded+=n>0;maximum=std::max(maximum,double(n));logsum+=std::log2(n+1.);}
    for(unsigned i=0;i<stages->size();++i){double n=0;for(const auto& w:*(*stages)[i].getAsArray())n+=*w.getAsInteger();auto reset=(*resets)[i].getAsInteger();if(!reset||*reset<0||*reset>n)throw std::runtime_error("invalid retained reset");fractions.push_back(*reset/std::max(1.,n));}
    for(auto& raw:qs){auto& q=*raw.getAsObject();auto& f=*q.getObject("features");int stage=int(*q.getInteger("stage"));auto src=*q.getString("control_source");bool native=src=="native_valid";
        f["native_log_target"]=native**f.getNumber("log2_target");f["native_load_log_target"]=native**f.getNumber("log2_logical_sinks")**f.getNumber("log2_target");f["native_sqrt_logical_sinks"]=native*std::sqrt(*f.getNumber("total_logical_sinks"));
        f["native_root_spread"]=native*std::sqrt(loaded);f["native_log_input_max_load"]=native*std::log2(maximum+1.);f["native_mean_log_input_load"]=native*logsum/std::max(1.,loaded);f["native_reset_one_fraction"]=native*(std::size_t(stage)<fractions.size()?fractions[stage]:0.);
        f["source_root_spread"]=std::sqrt(loaded);f["source_log_input_max_load"]=std::log2(maximum+1.);f["source_mean_log_input_load"]=logsum/std::max(1.,loaded);
        f["source_reset_one_fraction"]=(src=="port_iv"||src=="service_valid")?*inputReset/double(bits):fractions[std::min(std::max(stage-1,0),int(fractions.size())-1)];
    }return qs;
}
O evaluateCommonCentered(const O& model,const std::string& kind,const O& q,bool diagnostic) {
    try {
        if(model.getString("version")!="W345-common-centered-functions-v1"||model.getString("feature_version")!="common-prospective-mechanism-features-v1"||!diagnostic)throw std::runtime_error("centered successor requires explicit unqualified diagnostics");
        auto& models=object(model,"models");
        if(kind=="operator"){
            auto f=commonMechanismOperatorFeatures(q);auto& flow=object(q,"flow_context");
            if(flow.getString("profile_sha256")!=model.getString("operator_profile_sha256")||flow.getString("boundary_policy")!="unsigned-wide-family-hierarchy-v1")throw std::runtime_error("operator measurement profile mismatch");
            auto& local=object(q,"local_context");auto& spec=object(local,"producer");auto op=spec.getString("operation");
            std::string name=op=="add"||op=="adc"?"Add":op=="mux"?"Mux":"";if(name.empty())throw std::runtime_error("operator family outside successor");
            if(object(q,"topology").getInteger("operand_shared_input_roots").value_or(0))throw std::runtime_error("correlated operator operands require qualification");
            // Reuse the predecessor semantic gate through its frozen bundle.
            auto gate=evaluateOperatorSuccessor(object(model,"semantic_operator_model"),q,true);
            if(gate.getString("status")=="cost_uncovered")throw std::runtime_error(gate.getString("reason").value_or("operator semantic gate").str());
            auto& m=object(models,name.c_str());supported(object(m,"support"),f,*flow.getNumber("target_period_ps"));
            double value=centeredResponse(object(m,"function"),f);
            if(name=="Mux")return O{{"status","diagnostic_delay_only"},{"operator_delay_ps",value},{"qualified_for_public_D",false}};
            double reference=0;for(const auto& [n,w]:object(m,"reference_weights"))reference+=f.at(n.str())*num(w);
            return O{{"status","diagnostic_estimate"},{"comb_area_um2",value*reference},{"operator_delay_ps",linear(object(m,"retained_delay"),f)},{"qualified_for_public_D",false}};
        }
        if(kind=="G"){
            if(q.getString("launch_type").value_or("register")!="register"||q.getInteger("capture_banks").value_or(1)!=1||q.getInteger("consumer_count").value_or(0)!=0)throw std::runtime_error("unqualified reduction boundary");
            auto f=commonMechanismReductionFeatures(q);auto a=features(object(f,"area")),z=features(object(f,"intensive"));auto& m=object(models,"G");supported(object(m,"support"),a,*q.getNumber("target_ps"));
            double reference=0;for(const auto& [n,w]:object(m,"reference_area_weights"))reference+=a.at(n.str())*num(w);
            double area=reference*centeredResponse(object(m,"area"),z),delay=centeredResponse(object(m,"delay"),z);
            if(area<=0||delay<=0)throw std::runtime_error("nonpositive reduction price");
            return O{{"status","diagnostic_estimate"},{"comb_area_um2",area},{"operator_delay_ps",delay},{"qualified_for_public_D",false}};
        }
        if(kind=="S"){
            auto qs=commonMechanismStateFeatures(q);auto& m=object(models,"S");O paths;double T=*q.getNumber("target_ps"),log=std::log2(T/500.);
            for(const auto& [name,raw]:object(m,"functions")) {auto& entry=*raw.getAsObject();auto coeff=object(entry,"reserve_function").getArray("coefficients");if(!coeff||coeff->size()!=2)throw std::runtime_error("reserve dimensions");double guard=num((*coeff)[0])+num((*coeff)[1])*log/(1.+log);if(!std::isfinite(guard)||guard<0)throw std::runtime_error("invalid numerical state reserve");
                A prices;double maximum=0;
                for(const auto& rawq:qs){auto& c=*rawq.getAsObject();if(c.getString("path_class")!=name.str())continue;auto f=features(object(c,"features"));supported(object(object(m,"support"),name.str().c_str()),f,T);double value=centeredResponse(object(entry,"function"),f);if(value<=0)throw std::runtime_error("nonpositive control response");maximum=std::max(maximum,value);prices.push_back(O{{"stage",*c.getInteger("stage")},{"control_source",c.getString("control_source")->str()},{"required_ps",value},{"guarded_required_ps",value+guard}});}
                if(prices.empty())throw std::runtime_error("missing control source");paths[name]=O{{"required_ps",maximum},{"guarded_required_ps",maximum+guard},{"sources",std::move(prices)}};
            }
            return O{{"status","diagnostic_estimate"},{"paths",std::move(paths)},{"actual_arithmetic_S_qualified",false},{"price_qualification_granted",false}};
        }
        throw std::runtime_error("unknown centered model family");
    }catch(const std::exception& e){return O{{"status","cost_uncovered"},{"reason",std::string(e.what())}};}
}
}
