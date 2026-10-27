#pragma once

#include <cstdlib>
#include <iostream>
#include <string>

inline bool regressionCirctEnabled() {
    const char* value = std::getenv("VUL_REGRESSION_CIRCT");
    return !value || std::string(value) != "0";
}

inline void applyRegressionCirctPolicy(bool& use_circt) {
    if (!regressionCirctEnabled()) {
        use_circt = false;
        std::cout << "SKIP CIRCT checks: VUL_REGRESSION_CIRCT=0; native checks continue\n";
    }
}
