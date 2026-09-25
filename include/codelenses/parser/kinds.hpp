#pragma once

#include <string_view>

#include "codelenses/model.hpp"
#include "codelenses/worker/protocol.hpp"

namespace codelenses {

enum class SymbolKind {
    file,
    module,
    namespace_,
    package,
    class_,
    method,
    property,
    field,
    constructor,
    enum_,
    interface_,
    function,
    variable,
    constant,
    string,
    number,
    boolean,
    array,
    object,
    key,
    null_,
    enum_member,
    struct_,
    event,
    operator_,
    type_parameter,
    type_alias,
    macro,
    unknown,
};

[[nodiscard]] constexpr std::string_view to_string(SymbolKind kind) noexcept {
    switch (kind) {
    case SymbolKind::file:
        return "file";
    case SymbolKind::module:
        return "module";
    case SymbolKind::namespace_:
        return "namespace";
    case SymbolKind::package:
        return "package";
    case SymbolKind::class_:
        return "class";
    case SymbolKind::method:
        return "method";
    case SymbolKind::property:
        return "property";
    case SymbolKind::field:
        return "field";
    case SymbolKind::constructor:
        return "constructor";
    case SymbolKind::enum_:
        return "enum";
    case SymbolKind::interface_:
        return "interface";
    case SymbolKind::function:
        return "function";
    case SymbolKind::variable:
        return "variable";
    case SymbolKind::constant:
        return "constant";
    case SymbolKind::string:
        return "string";
    case SymbolKind::number:
        return "number";
    case SymbolKind::boolean:
        return "boolean";
    case SymbolKind::array:
        return "array";
    case SymbolKind::object:
        return "object";
    case SymbolKind::key:
        return "key";
    case SymbolKind::null_:
        return "null";
    case SymbolKind::enum_member:
        return "enum_member";
    case SymbolKind::struct_:
        return "struct";
    case SymbolKind::event:
        return "event";
    case SymbolKind::operator_:
        return "operator";
    case SymbolKind::type_parameter:
        return "type_parameter";
    case SymbolKind::type_alias:
        return "type_alias";
    case SymbolKind::macro:
        return "macro";
    case SymbolKind::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] constexpr SymbolKind symbol_kind_from_string(std::string_view str) noexcept {
    if (str == "file")
        return SymbolKind::file;
    if (str == "module")
        return SymbolKind::module;
    if (str == "namespace")
        return SymbolKind::namespace_;
    if (str == "package")
        return SymbolKind::package;
    if (str == "class")
        return SymbolKind::class_;
    if (str == "method")
        return SymbolKind::method;
    if (str == "property")
        return SymbolKind::property;
    if (str == "field")
        return SymbolKind::field;
    if (str == "constructor")
        return SymbolKind::constructor;
    if (str == "enum")
        return SymbolKind::enum_;
    if (str == "interface")
        return SymbolKind::interface_;
    if (str == "function")
        return SymbolKind::function;
    if (str == "variable")
        return SymbolKind::variable;
    if (str == "constant")
        return SymbolKind::constant;
    if (str == "string")
        return SymbolKind::string;
    if (str == "number")
        return SymbolKind::number;
    if (str == "boolean")
        return SymbolKind::boolean;
    if (str == "array")
        return SymbolKind::array;
    if (str == "object")
        return SymbolKind::object;
    if (str == "key")
        return SymbolKind::key;
    if (str == "null")
        return SymbolKind::null_;
    if (str == "enum_member")
        return SymbolKind::enum_member;
    if (str == "struct")
        return SymbolKind::struct_;
    if (str == "event")
        return SymbolKind::event;
    if (str == "operator")
        return SymbolKind::operator_;
    if (str == "type_parameter")
        return SymbolKind::type_parameter;
    if (str == "type_alias")
        return SymbolKind::type_alias;
    if (str == "macro")
        return SymbolKind::macro;
    return SymbolKind::unknown;
}

[[nodiscard]] constexpr SymbolKind symbol_kind_from_node_kind(NodeKind k) noexcept {
    switch (k) {
    case NodeKind::file:
        return SymbolKind::file;
    case NodeKind::namespace_:
        return SymbolKind::namespace_;
    case NodeKind::class_:
        return SymbolKind::class_;
    case NodeKind::struct_:
        return SymbolKind::struct_;
    case NodeKind::interface_:
        return SymbolKind::interface_;
    case NodeKind::function:
        return SymbolKind::function;
    case NodeKind::method:
        return SymbolKind::method;
    case NodeKind::variable:
        return SymbolKind::variable;
    case NodeKind::module:
        return SymbolKind::module;
    case NodeKind::package:
        return SymbolKind::package;
    }
    return SymbolKind::unknown;
}

enum class RelationKind {
    contains,
    calls,
    imports,
    includes,
    inherits,
    implements,
    overrides,
    instantiates,
    throws,
    returns,
    type_of,
    unknown,
};

[[nodiscard]] constexpr std::string_view to_string(RelationKind kind) noexcept {
    switch (kind) {
    case RelationKind::contains:
        return "contains";
    case RelationKind::calls:
        return "calls";
    case RelationKind::imports:
        return "imports";
    case RelationKind::includes:
        return "includes";
    case RelationKind::inherits:
        return "inherits";
    case RelationKind::implements:
        return "implements";
    case RelationKind::overrides:
        return "overrides";
    case RelationKind::instantiates:
        return "instantiates";
    case RelationKind::throws:
        return "throws";
    case RelationKind::returns:
        return "returns";
    case RelationKind::type_of:
        return "type_of";
    case RelationKind::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] constexpr RelationKind relation_kind_from_string(std::string_view str) noexcept {
    if (str == "contains")
        return RelationKind::contains;
    if (str == "calls")
        return RelationKind::calls;
    if (str == "imports")
        return RelationKind::imports;
    if (str == "includes")
        return RelationKind::includes;
    if (str == "inherits")
        return RelationKind::inherits;
    if (str == "implements")
        return RelationKind::implements;
    if (str == "overrides")
        return RelationKind::overrides;
    if (str == "instantiates")
        return RelationKind::instantiates;
    if (str == "throws")
        return RelationKind::throws;
    if (str == "returns")
        return RelationKind::returns;
    if (str == "type_of")
        return RelationKind::type_of;
    return RelationKind::unknown;
}

[[nodiscard]] constexpr RelationKind
relation_kind_from_fact_kind(worker::FactKind fact_kind) noexcept {
    switch (fact_kind) {
    case worker::FactKind::reference:
        return RelationKind::unknown;
    case worker::FactKind::call:
        return RelationKind::calls;
    case worker::FactKind::include:
        return RelationKind::includes;
    case worker::FactKind::import:
        return RelationKind::imports;
    case worker::FactKind::inheritance:
        return RelationKind::inherits;
    case worker::FactKind::implementation:
        return RelationKind::implements;
    case worker::FactKind::symbol:
    case worker::FactKind::declaration:
    case worker::FactKind::scope:
        return RelationKind::unknown;
    }
    return RelationKind::unknown;
}

enum class OccurrenceKind {
    definition,
    declaration,
    reference,
    implementation,
    override_,
    import_,
    include_,
    unknown,
};

[[nodiscard]] constexpr std::string_view to_string(OccurrenceKind kind) noexcept {
    switch (kind) {
    case OccurrenceKind::definition:
        return "definition";
    case OccurrenceKind::declaration:
        return "declaration";
    case OccurrenceKind::reference:
        return "reference";
    case OccurrenceKind::implementation:
        return "implementation";
    case OccurrenceKind::override_:
        return "override";
    case OccurrenceKind::import_:
        return "import";
    case OccurrenceKind::include_:
        return "include";
    case OccurrenceKind::unknown:
        return "unknown";
    }
    return "unknown";
}

[[nodiscard]] constexpr OccurrenceKind occurrence_kind_from_string(std::string_view str) noexcept {
    if (str == "definition")
        return OccurrenceKind::definition;
    if (str == "declaration")
        return OccurrenceKind::declaration;
    if (str == "reference")
        return OccurrenceKind::reference;
    if (str == "implementation")
        return OccurrenceKind::implementation;
    if (str == "override")
        return OccurrenceKind::override_;
    if (str == "import")
        return OccurrenceKind::import_;
    if (str == "include")
        return OccurrenceKind::include_;
    return OccurrenceKind::unknown;
}

} // namespace codelenses
