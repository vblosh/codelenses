#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "codelenses/diagnostic.hpp"
#include "codelenses/identifier.hpp"
#include "codelenses/language.hpp"
#include "codelenses/range.hpp"
#include "codelenses/result.hpp"
#include "codelenses/status.hpp"

namespace codelenses {

enum class NodeKind {
    file,
    namespace_,
    class_,
    struct_,
    interface_,
    function,
    method,
    variable,
    module,
    package,
    macro,
    enum_,
    type_alias,
    field,
    enum_member
};

[[nodiscard]] std::string_view to_string(NodeKind value) noexcept;

} // namespace codelenses
