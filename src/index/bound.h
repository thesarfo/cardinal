#pragma once

#include "common/value.h"

namespace cardinal {

// One end of a range over keys. An absent bound (std::optional left empty) means open on that side.
struct Bound {
    Value value;
    bool inclusive = true;
};

}  // namespace cardinal
