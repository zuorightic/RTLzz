#include "backend/arithmetic_control_successor.hpp"
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
using F = std::map<std::string, double>;
void fail(const char* message) { throw std::runtime_error(message); }
double number(const O& o, const char* key) {
    auto n = o.getNumber(key);
    if (!n || !std::isfinite(*n)) fail("finite S control number required");
    return *n;
}
const A& array(const O& o, const char* key) {
    auto a = o.getArray(key); if (!a) fail("S control array required"); return *a;
}
int integer(const llvm::json::Value& v, int low, int high) {
    auto n = v.getAsInteger();
    if (!n || *n < low || *n > high) fail("bounded S control integer required");
    return static_cast<int>(*n);
}
O json(const F& f) { O o; for (const auto& [n, v] : f) o[n] = v; return o; }
}
A prospectiveControlFeatures(const O& s) {
    const auto& roots = array(s, "input_widths");
    const auto& groups = array(s, "stage_words");
    if (roots.empty() || roots.size() > 4096 || groups.empty() || groups.size() > 4096)
        fail("bounded prospective state/control structure required");
    auto ev = s.get("external_bits"), mv = s.get("service_mode");
    if (!ev || !mv) fail("explicit S control semantics required");
    int external = integer(*ev, 0, 1048576), mode = integer(*mv, 0, 2);
    double T = number(s, "target_ps");
    if (T < 500 || T > 3000) fail("S control numerical target outside diagnostic domain");
    double input = 0, native = 0;
    for (const auto& r : roots) input += integer(r, 1, 4096);
    std::vector<std::vector<int>> words;
    for (const auto& group : groups) {
        auto a = group.getAsArray(); if (!a || a->size() > 4096) fail("retained words required");
        words.emplace_back();
        for (const auto& w : *a) { auto n = integer(w, 1, 4096); native += n; words.back().push_back(n); }
    }
    if (input + native + external > 1048576) fail("prospective state/control size bound exceeded");
    double total = input + 1 + native + words.size() + external + 1;
    bool held = mode != 2; A result;
    auto append = [&](const char* path, const char* source, unsigned stage, double data,
                      double valid, double hold, bool captureHold, double enable = 0) {
        const auto& group = stage < words.size() ? words[stage] : std::vector<int>{};
        double load = data + valid + hold, n = std::string(source) == "native_valid";
        double logload = std::log2(load + 1), pressure = 1500 / T;
        F f{{"one",1}, {"target_ps",T}, {"log2_target",std::log2(T/500)}, {"clock_pressure",pressure},
            {"enabled_data_sinks",data}, {"valid_propagation_sinks",valid}, {"self_hold_sinks",hold},
            {"total_logical_sinks",load}, {"log2_logical_sinks",logload},
            {"launch_is_service",std::string(source)=="service_valid"}, {"launch_has_hold",hold>0},
            {"capture_has_hold",captureHold}, {"capture_enable_logical_sinks",enable},
            {"log2_capture_enable_load",std::log2(enable+1)}, {"sqrt_logical_sinks",std::sqrt(load)},
            {"pressure_logical_sinks",logload*pressure}, {"native_launch",n},
            {"native_logical_load",n*logload}, {"native_pressure_logical_load",n*logload*pressure},
            {"total_FF_bits",total}, {"sqrt_total_FF_bits",std::sqrt(total)},
            {"enabled_word_count",double(group.size())},
            {"max_enabled_word_bits",group.empty()?0.:double(*std::max_element(group.begin(),group.end()))},
            {"native_footprint",n*std::sqrt(total)}, {"native_word_load",n*group.size()},
            {"native_log_word_load",n*std::log2(group.size()+1.)}, {"load_fraction",load/std::max(1.,native)},
            {"pressure_native_footprint",n*std::sqrt(total)*pressure}};
        result.push_back(O{{"path_class",path},{"control_source",source},{"stage",stage},
                           {"features",json(f)},{"mapped_capacitance_qualified",false},
                           {"price_qualification_granted",false}});
    };
    append("input_valid","port_iv",0,0,1,0,held,mode==1?input+1:held?1:0);
    for (unsigned i=0;i<words.size();++i) {
        double bits=0; for(auto w:words[i]) bits+=w;
        for(auto path:{"native_valid","native_enable"})
            append(path,i==0?"service_valid":"native_valid",i,bits,1,i==0&&held,false);
    }
    append("external_valid","native_valid",words.size(),0,1,0,false);
    return result;
}
O evaluateControlSuccessor(const O& model,const O& s,bool diagnostic) {
    try {
        if(model.getString("version")!="per-source-control-features-v3-bank-geometry")
            fail("S control feature identity mismatch");
        if(!diagnostic) fail("S successor has no public price qualification");
        auto queries=prospectiveControlFeatures(s);auto functions=model.getObject("functions");
        if(!functions||functions->size()!=4) fail("incomplete S control functions");
        const std::set<std::string> allowed{"native_valid","native_enable","input_valid","external_valid"};
        O paths;
        for(const auto& item:*functions) {
            auto name=item.first.str();auto entry=item.second.getAsObject();
            if(!allowed.count(name)||!entry) fail("unknown S control path family");
            double reserve=number(*entry,"reserve_ps");
            if(reserve<0) fail("invalid S control reserve");
            auto m=entry->getObject("function");if(!m) fail("S control function required");
            const auto& names=array(*m,"features");const auto& coeff=array(*m,"coefficients");
            if(names.size()!=coeff.size()) fail("S control coefficient mismatch");
            A prices;double maximum=0;
            for(const auto& query:queries) {
                const auto& q=*query.getAsObject();if(q.getString("path_class")!=name) continue;
                double value=m->get("intercept")?number(*m,"intercept"):0.;
                const auto& f=*q.getObject("features");
                for(unsigned i=0;i<names.size();++i) {
                    auto n=names[i].getAsString();auto c=coeff[i].getAsNumber();
                    if(!n||!c||!std::isfinite(*c)) fail("invalid S control coefficient");
                    value+=number(f,n->str().c_str())**c;
                }
                if(!std::isfinite(value)) fail("nonfinite S control estimate");
                value=std::max(0.,value);maximum=std::max(maximum,value);
                prices.push_back(O{{"stage",*q.getInteger("stage")},{"control_source",q.getString("control_source")->str()},
                    {"required_ps",value},{"guarded_required_ps",value+reserve}});
            }
            if(prices.empty()) fail("missing prospective S control source");
            paths[name]=O{{"required_ps",maximum},{"guarded_required_ps",maximum+reserve},{"sources",std::move(prices)}};
        }
        return O{{"status","diagnostic_estimate"},{"paths",std::move(paths)},
                 {"actual_arithmetic_S_qualified",false},{"price_qualification_granted",false}};
    } catch(const std::exception& error) {
        return O{{"status","cost_uncovered"},{"reason",std::string(error.what())}};
    }
}
}
