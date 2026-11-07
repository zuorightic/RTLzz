#pragma once
#include "arithmetic_general_state_fixture.hpp"
#include "backend/arithmetic_joint.hpp"

static std::string jointSource(const std::string& kind,const std::vector<int>& widths,const std::vector<int>& outputs){
    std::ostringstream s;s<<"#include <fixint.hpp>\n#include <arithmetic.hpp>\nInt<1> iv,ov;\n#pragma input_port iv\n#pragma output_port ov\n";
    for(std::size_t j=0;j<widths.size();++j)s<<"Int<"<<widths[j]<<"> x"<<j<<";\n#pragma input_port x"<<j<<"\n";
    for(std::size_t j=0;j<outputs.size();++j)s<<"Int<"<<outputs[j]<<"> value"<<j<<";\n#pragma output_port value"<<j<<"\n";
    s<<"void JointReference(){\n";
    if(kind=="mul32_shared"||kind=="mulreduce"){
        auto w=outputs[0];s<<"Int<"<<w<<"> p=x0*x1,q=x2*x3,t=x0*x2;value0=p+t;value1=q^t;\n";
    }else if(kind=="div16")s<<"Int<16> safe=x1==Int<16>(0)?Int<16>(1):x1;value0=(x0/safe)^x0;value1=(x0%safe)^x1;\n";
    else if(kind=="div32_select")s<<"Int<32> a=x0+x2,b=x1^x3,safe=b==Int<32>(0)?Int<32>(1):b,q=a/safe,r=a%safe;value0=(x4!=Int<1>(0))?(q^x2):(r+x3);value1=r^x0;\n";
    else if(kind=="div32_three")s<<"Int<32> a=x0^x2,safe=x1==Int<32>(0)?Int<32>(1):x1;value0=(a/safe)+x2;value1=(a%safe)^x0;\n";
    else if(kind=="fp32_two")s<<"value0=vularith::F32MulRNE(x0,x1);value1=(x0^x1)+x0;\n";
    else if(kind=="fp32")s<<"Int<32> a=x0^x2;value0=vularith::F32MulRNE(a,x1);value1=x2+x0;\n";
    else if(kind=="fp32_select")s<<"Int<32> a=x0^x2;value0=vularith::F32MulRNE(a,x1);value1=(x3!=Int<1>(0))?(x2+x0):(x1^x2);\n";
    else if(kind=="addmux")s<<"Int<32> mix=Int<32>(x0+x1)^x2;value0=(x3!=Int<1>(0))?mix:x1;value1=(x0^x2)+x1;\n";
    else throw std::runtime_error("unregistered synthetic reference core");
    s<<"ov=iv;}\n";return s.str();
}
static Program compileJointReference(const std::filesystem::path& root,const std::string& source){
    pipelinev2::PipelineConfig c;c.source_name="registered_synthetic_G.logic.cpp";c.source_text=source;c.top_function="JointReference";
    c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.output_kind=pipelinev2::OutputKind::Beir;
    auto result=pipelinev2::compile(c);check(result.ok()&&result.beir_program.has_value(),"synthetic native reference: "+result.error);
    return *result.beir_program;
}
static std::vector<int> integerArray(const O& o,const char* key){std::vector<int> r;for(const auto& v:*o.getArray(key))r.push_back(int(*v.getAsInteger()));return r;}
