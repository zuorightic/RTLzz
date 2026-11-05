#include "pipelinev2/PipelineV2.h"
#include "backend/rtlgen.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>

int main(int argc,char** argv) {
    if(argc!=3)return 2;
    std::filesystem::path root=argv[1],out=argv[2];
    std::filesystem::create_directories(out);
    pred::pipelinev2::PipelineConfig c;
    c.source_name="m2-isolated-fp32.logic.cpp";
    c.source_text="#include <fixint.hpp>\n#include <arithmetic.hpp>\nInt<32> a,b;Int<37> value;\n#pragma input_port a\n#pragma input_port b\n#pragma output_port value\nvoid AtomicFp(){value=vularith::F32MulRNE(a,b);}\n";
    c.top_function="AtomicFp";c.clang_args={"-std=c++20","-I"+(root/"vullib").string()};c.use_circt=false;
    auto result=pred::pipelinev2::compile(c);
    if(!result.ok()||!result.beir_program){std::cerr<<result.error;return 1;}
    const auto text=pred::rtlgen::emitSystemVerilog(*result.beir_program);
    if(std::filesystem::exists(out/"AtomicFp.sv"))return 3;
    std::ofstream(out/"AtomicFp.sv")<<text;
    std::ofstream(out/"AtomicFp.logic.cpp")<<*c.source_text;
    std::cout<<"Isolated complete native binary32 atom exported\n";
}
