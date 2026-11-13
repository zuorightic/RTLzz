#include "backend/arithmetic_operator_successor.hpp"
#include <stdexcept>
namespace pred::beir::arithmetic {
std::map<std::string,double> successorOperatorFeatures(const llvm::json::Object& q){
    auto f=wideFunctionQueryFeatures(q);
    f["compare_order_work"]=(1.-f.at("predicate_eq"))*f.at("operand_live_max");
    return f;
}
llvm::json::Object evaluateOperatorSuccessor(const llvm::json::Object& model,const llvm::json::Object& q,bool diagnostic){
    using O=llvm::json::Object;using A=llvm::json::Array;
    try{
        if(model.getString("version")!="common-wide-operator-functions-v3-compare-work"||
           model.getString("feature_version")!="wide-function-features-v3-disjoint-compare-work")
            throw std::runtime_error("operator successor feature identity mismatch");
        O adapted(model);adapted["version"]="common-wide-operator-functions-v2";
        auto functions=adapted.getObject("functions");if(!functions)throw std::runtime_error("missing successor functions");
        for(auto& item:*functions){auto family=item.second.getAsObject();if(!family)throw std::runtime_error("invalid successor function");
            for(auto key:{"delay_ps","area_um2"}){
                auto function=family->getObject(key);if(!function)throw std::runtime_error("missing successor linear function");
                auto names=function->getArray("features"),coeff=function->getArray("coefficients");
                if(!names||!coeff||names->size()!=coeff->size())throw std::runtime_error("successor coefficient mismatch");
                std::map<std::string,double> terms;
                for(std::size_t i=0;i<names->size();++i){auto name=(*names)[i].getAsString();auto value=(*coeff)[i].getAsNumber();
                    if(!name||!value)throw std::runtime_error("invalid successor coefficient");
                    if(*name=="compare_order_work"){terms["operand_live_max"]+=*value;terms["compare_eq_work"]-=*value;}
                    else terms[name->str()]+=*value;
                }
                A n,c;for(const auto& [name,value]:terms){n.push_back(name);c.push_back(value);}
                (*function)["features"]=std::move(n);(*function)["coefficients"]=std::move(c);
            }
        }
        auto answer=evaluateWideFunctionModel(adapted,q,diagnostic);
        if(answer.getString("status")=="covered"||answer.getString("status")=="diagnostic_estimate"){
            O f;for(const auto& [n,x]:successorOperatorFeatures(q))f[n]=x;answer["features"]=std::move(f);
        }
        return answer;
    }catch(const std::exception& error){return O{{"status","cost_uncovered"},{"reason",std::string(error.what())}};}
}
}
