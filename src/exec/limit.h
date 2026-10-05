#pragma once

#include <cstdint>
#include <memory>

#include "exec/operator.h"

namespace cardinal {

// Passes on the first `count` rows and then stops. It never asks its input for a
// row it won't pass on, so LIMIT 0 reads nothing and the steps below do no more
// work than they have to.
class Limit : public Operator {
public:
    const char* name() const override { return "Limit"; }
    Limit(std::unique_ptr<Operator> input, std::int64_t count)
        : input_(std::move(input)), remaining_(count) {}

    std::vector<const Operator*> children() const override { return {input_.get()}; }

protected:
    std::optional<Row> produce() override;

private:
    std::unique_ptr<Operator> input_;
    std::int64_t remaining_;
};

}  // namespace cardinal
