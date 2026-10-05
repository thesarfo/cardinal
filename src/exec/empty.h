#pragma once

#include "exec/operator.h"

namespace cardinal {

// Gives no rows and reads nothing.
class Empty : public Operator {
public:
    const char* name() const override { return "Empty"; }

protected:
    std::optional<Row> produce() override { return std::nullopt; }
};

}  // namespace cardinal
