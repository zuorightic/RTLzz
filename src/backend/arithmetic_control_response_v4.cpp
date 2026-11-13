#include "backend/arithmetic_control_response_v4.hpp"
#include "backend/arithmetic_control_successor.hpp"
#include <algorithm>
#include <cmath>
#include <set>
#include <stdexcept>
#include <vector>
namespace pred::beir::arithmetic {
llvm::json::Object evaluateControlResponseV4(const llvm::json::Object& model,
    const llvm::json::Object& structure,bool diagnostic) {
    using O=llvm::json::Object;using A=llvm::json::Array;
    try {
        bool shared=model.getString("version")=="shared-state-control-response-v8";
        bool distribution=shared||model.getString("version")=="per-source-control-features-v5-root-distribution-reset";
        if(!distribution&&model.getString("version")!="per-source-control-features-v4-native-T-load")throw std::runtime_error("S response feature identity mismatch");
        if(!diagnostic)throw std::runtime_error("S response successor lacks public qualification");
        auto queries=prospectiveControlFeatures(structure);
        double rootSpread=0,rootMaxLog=0,rootMeanLog=0;
        std::vector<double> resetFractions;
        if(distribution) {
            auto sinks=structure.getArray("input_sink_inputs"),resets=structure.getArray("stage_reset_one_bits");
            auto roots=structure.getArray("input_widths"),stages=structure.getArray("stage_words");
            if(!sinks||!resets||resets->size()!=stages->size()||sinks->size()>1048576)throw std::runtime_error("prospective S connection/reset structure required");
            std::size_t inputBits=0;for(const auto& w:*roots)inputBits+=*w.getAsInteger();
            std::vector<unsigned> counts(inputBits);
            for(const auto& raw:*sinks) {
                auto a=raw.getAsArray();if(!a||a->empty())throw std::runtime_error("logical S sink inputs required");std::set<unsigned> unique;
                for(const auto& rawBit:*a) {auto b=rawBit.getAsInteger();if(!b||*b<0||std::size_t(*b)>=inputBits)throw std::runtime_error("logical S sink outside root bits");unique.insert(*b);}
                for(auto b:unique)++counts[b];
            }
            double loaded=0,maximum=0,logsum=0;
            for(auto n:counts){loaded+=n>0;maximum=std::max(maximum,double(n));logsum+=std::log2(n+1.);}
            rootSpread=std::sqrt(loaded);rootMaxLog=std::log2(maximum+1.);rootMeanLog=logsum/std::max(1.,loaded);
            for(unsigned i=0;i<stages->size();++i) {
                double bits=0;for(const auto& w:*(*stages)[i].getAsArray())bits+=*w.getAsInteger();auto ones=(*resets)[i].getAsInteger();
                if(!ones||*ones<0||*ones>bits)throw std::runtime_error("invalid retained S reset polarity");resetFractions.push_back(*ones/std::max(1.,bits));
            }
        }
        for(auto& raw:queries) {
            auto& q=*raw.getAsObject();auto& f=*q.getObject("features");double native=q.getString("control_source")=="native_valid";
            f["native_log_target"]=native**f.getNumber("log2_target");
            f["native_load_log_target"]=native**f.getNumber("log2_logical_sinks")**f.getNumber("log2_target");
            f["native_sqrt_logical_sinks"]=native*std::sqrt(*f.getNumber("total_logical_sinks"));
            if(distribution) {
                auto stage=*q.getInteger("stage");
                f["native_root_spread"]=native*rootSpread;
                f["native_log_input_max_load"]=native*rootMaxLog;
                f["native_mean_log_input_load"]=native*rootMeanLog;
                f["native_reset_one_fraction"]=native*(std::size_t(stage)<resetFractions.size()?resetFractions[stage]:0.);
            }
        }
        auto functions=model.getObject("functions");const std::set<std::string> allowed{"native_valid","native_enable","input_valid","external_valid"};
        if(!functions||functions->size()!=4)throw std::runtime_error("incomplete S response functions");O paths;
        for(const auto& item:*functions) {
            auto name=item.first.str();auto entry=item.second.getAsObject();if(!entry||!allowed.count(name))throw std::runtime_error("unknown S response family");
            auto reserve=entry->getNumber("reserve_ps");auto m=entry->getObject("function");
            if(!reserve||!std::isfinite(*reserve)||*reserve<0||!m)throw std::runtime_error("invalid S response reserve");
            double guard=*reserve;
            if(shared) {
                auto kind=m->getString("kind");
                if(!kind||(*kind!="linear"&&*kind!="positive_log_linear"))throw std::runtime_error("unknown shared S function kind");
                if(auto r=entry->getObject("reserve_function")) {
                    auto rn=r->getArray("features"),rc=r->getArray("coefficients");
                    if(r->getString("kind")!="nonnegative_linear"||!rn||!rc||rn->size()!=rc->size())throw std::runtime_error("S reserve function identity/dimensions");
                    double log=std::log2(*structure.getNumber("target_ps")/500.);guard=0.;std::set<std::string> seen;
                    for(unsigned i=0;i<rn->size();++i) {
                        auto n=(*rn)[i].getAsString();auto c=(*rc)[i].getAsNumber();
                        if(!n||!c||!std::isfinite(*c)||*c<0||!seen.insert(n->str()).second)throw std::runtime_error("invalid numerical S reserve");
                        double x=*n=="one"?1.:*n=="control_log_relaxation"?log/(1.+log):-1.;
                        if(x<0)throw std::runtime_error("unknown S reserve feature");guard+=x**c;
                    }
                    if(!std::isfinite(guard)||guard<0)throw std::runtime_error("invalid numerical S reserve");
                }
            }
            auto names=m->getArray("features"),coeff=m->getArray("coefficients");if(!names||!coeff||names->size()!=coeff->size())throw std::runtime_error("S response coefficient dimensions");
            A prices;double maximum=0;
            for(const auto& raw:queries) {
                auto& q=*raw.getAsObject();if(q.getString("path_class")!=name)continue;auto& f=*q.getObject("features");double y=0.;long double accurate=m->getNumber("intercept").value_or(0.);
                for(unsigned i=0;i<names->size();++i) {
                    auto n=(*names)[i].getAsString();auto c=(*coeff)[i].getAsNumber();if(!n||!c||!std::isfinite(*c))throw std::runtime_error("S response coefficient invalid");
                    auto v=f.getNumber(*n);if(!v)throw std::runtime_error("S response feature missing");double product=*v**c;if(shared)accurate+=product;else y+=product;
                }
                if(shared)y=double(accurate);else y+=m->getNumber("intercept").value_or(0.);
                if(shared&&m->getString("kind")=="positive_log_linear")y=std::exp(y);
                else y=std::max(0.,y);
                if(!std::isfinite(y))throw std::runtime_error("nonfinite S response");maximum=std::max(maximum,y);
                prices.push_back(O{{"stage",*q.getInteger("stage")},{"control_source",q.getString("control_source")->str()},
                                  {"required_ps",y},{"guarded_required_ps",y+guard}});
            }
            if(prices.empty())throw std::runtime_error("S response source missing");
            paths[name]=O{{"required_ps",maximum},{"guarded_required_ps",maximum+guard},{"sources",std::move(prices)}};
        }
        return O{{"status","diagnostic_estimate"},{"paths",std::move(paths)},
                 {"actual_arithmetic_S_qualified",false},{"price_qualification_granted",false}};
    }catch(const std::exception& e){return O{{"status","cost_uncovered"},{"reason",std::string(e.what())}};}
}
}
