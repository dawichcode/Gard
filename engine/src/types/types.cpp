#include "types/types.h"

namespace gard {

// --- Type base ---

bool Type::equals(const Type* other) const {
    if (!other) return false;
    return category == other->category && name == other->name;
}

bool Type::isAssignableTo(const Type* target) const {
    if (!target) return false;
    if (target->isDynamic()) return true;
    if (target->isError() || this->isError()) return true;
    if (target->isUnknown() || this->isUnknown()) return true;
    return equals(target);
}

bool Type::isNumeric() const {
    return isPrimitive() && (name == "int" || name == "long" || name == "short" ||
                             name == "double" || name == "float" || name == "uint" || name == "bigint");
}

bool Type::isIntegral() const {
    return isPrimitive() && (name == "int" || name == "long" || name == "short" ||
                             name == "uint" || name == "bigint");
}

bool Type::isFloating() const {
    return isPrimitive() && (name == "double" || name == "float");
}

// --- Collection types ---

bool ArrayType::equals(const Type* other) const {
    if (auto* arr = dynamic_cast<const ArrayType*>(other)) {
        return elementType->equals(arr->elementType.get());
    }
    return false;
}

bool ArrayType::isAssignableTo(const Type* target) const {
    if (target->isDynamic()) return true;
    if (auto* arr = dynamic_cast<const ArrayType*>(target)) {
        return elementType->isAssignableTo(arr->elementType.get());
    }
    return false;
}

bool MapType::equals(const Type* other) const {
    if (auto* m = dynamic_cast<const MapType*>(other)) {
        return keyType->equals(m->keyType.get()) && valueType->equals(m->valueType.get());
    }
    return false;
}

bool MapType::isAssignableTo(const Type* target) const {
    if (target->isDynamic()) return true;
    if (auto* m = dynamic_cast<const MapType*>(target)) {
        return keyType->isAssignableTo(m->keyType.get()) &&
               valueType->isAssignableTo(m->valueType.get());
    }
    return false;
}

bool SetType::equals(const Type* other) const {
    if (auto* s = dynamic_cast<const SetType*>(other)) {
        return elementType->equals(s->elementType.get());
    }
    return false;
}

bool SetType::isAssignableTo(const Type* target) const {
    if (target->isDynamic()) return true;
    if (auto* s = dynamic_cast<const SetType*>(target)) {
        return elementType->isAssignableTo(s->elementType.get());
    }
    return false;
}

// --- Tuple type ---

std::string TupleTypeInfo::toString() const {
    std::string result = "(";
    for (size_t i = 0; i < elements.size(); i++) {
        if (i > 0) result += ", ";
        result += elements[i]->toString();
    }
    return result + ")";
}

bool TupleTypeInfo::equals(const Type* other) const {
    if (auto* t = dynamic_cast<const TupleTypeInfo*>(other)) {
        if (elements.size() != t->elements.size()) return false;
        for (size_t i = 0; i < elements.size(); i++) {
            if (!elements[i]->equals(t->elements[i].get())) return false;
        }
        return true;
    }
    return false;
}

bool TupleTypeInfo::isAssignableTo(const Type* target) const {
    if (target->isDynamic()) return true;
    return equals(target);
}

// --- Nullable type ---

bool NullableTypeWrapper::equals(const Type* other) const {
    if (auto* n = dynamic_cast<const NullableTypeWrapper*>(other)) {
        return inner->equals(n->inner.get());
    }
    return false;
}

bool NullableTypeWrapper::isAssignableTo(const Type* target) const {
    if (target->isDynamic()) return true;
    // T? is assignable to T? (same nullable)
    if (auto* n = dynamic_cast<const NullableTypeWrapper*>(target)) {
        return inner->isAssignableTo(n->inner.get());
    }
    // T? is NOT assignable to T (need null check)
    return false;
}

// --- Function type ---

std::string FunctionTypeInfo::toString() const {
    std::string result = "(";
    for (size_t i = 0; i < paramTypes.size(); i++) {
        if (i > 0) result += ", ";
        result += paramTypes[i]->toString();
    }
    result += ") => " + returnType->toString();
    return result;
}

bool FunctionTypeInfo::equals(const Type* other) const {
    if (auto* f = dynamic_cast<const FunctionTypeInfo*>(other)) {
        if (paramTypes.size() != f->paramTypes.size()) return false;
        if (!returnType->equals(f->returnType.get())) return false;
        for (size_t i = 0; i < paramTypes.size(); i++) {
            if (!paramTypes[i]->equals(f->paramTypes[i].get())) return false;
        }
        return true;
    }
    return false;
}

bool FunctionTypeInfo::isAssignableTo(const Type* target) const {
    if (target->isDynamic()) return true;
    return equals(target);
}

// --- Class type ---

bool ClassType::equals(const Type* other) const {
    if (auto* c = dynamic_cast<const ClassType*>(other)) {
        return className == c->className;
    }
    return false;
}

bool ClassType::isAssignableTo(const Type* target) const {
    if (target->isDynamic()) return true;
    // Same class
    if (auto* c = dynamic_cast<const ClassType*>(target)) {
        if (className == c->className) return true;
        // Check inheritance: this class extends target class
        if (baseClass && baseClass->isAssignableTo(target)) return true;
    }
    // Check interface satisfaction
    if (auto* iface = dynamic_cast<const InterfaceType*>(target)) {
        for (auto& impl : interfaces) {
            if (impl == iface->interfaceName) return true;
        }
    }
    return false;
}

// --- Interface type ---

bool InterfaceType::equals(const Type* other) const {
    if (auto* i = dynamic_cast<const InterfaceType*>(other)) {
        return interfaceName == i->interfaceName;
    }
    return false;
}

bool InterfaceType::isAssignableTo(const Type* target) const {
    if (target->isDynamic()) return true;
    return equals(target);
}

// --- Generic instantiation ---

std::string GenericInstType::toString() const {
    std::string result = baseName + "<";
    for (size_t i = 0; i < typeArgs.size(); i++) {
        if (i > 0) result += ", ";
        result += typeArgs[i]->toString();
    }
    return result + ">";
}

bool GenericInstType::equals(const Type* other) const {
    if (auto* g = dynamic_cast<const GenericInstType*>(other)) {
        if (baseName != g->baseName) return false;
        if (typeArgs.size() != g->typeArgs.size()) return false;
        for (size_t i = 0; i < typeArgs.size(); i++) {
            if (!typeArgs[i]->equals(g->typeArgs[i].get())) return false;
        }
        return true;
    }
    return false;
}

bool GenericInstType::isAssignableTo(const Type* target) const {
    if (target->isDynamic()) return true;
    return equals(target);
}

// --- Generic param type ---

bool GenericParamType::equals(const Type* other) const {
    if (auto* g = dynamic_cast<const GenericParamType*>(other)) {
        return paramName == g->paramName;
    }
    return false;
}

bool GenericParamType::isAssignableTo(const Type* target) const {
    if (target->isDynamic()) return true;
    // A generic param satisfies its constraint
    if (constraint && target->equals(constraint.get())) return true;
    return equals(target);
}

// --- Type Registry ---

TypeRegistry::TypeRegistry() {
    int_ = std::make_shared<PrimitiveType>("int");
    long_ = std::make_shared<PrimitiveType>("long");
    short_ = std::make_shared<PrimitiveType>("short");
    double_ = std::make_shared<PrimitiveType>("double");
    float_ = std::make_shared<PrimitiveType>("float");
    char_ = std::make_shared<PrimitiveType>("char");
    boolean_ = std::make_shared<PrimitiveType>("boolean");
    string_ = std::make_shared<PrimitiveType>("string");
    void_ = std::make_shared<VoidType>();
    null_ = std::make_shared<NullType>();
    dynamic_ = std::make_shared<DynamicType>();
    unknown_ = std::make_shared<UnknownType>();
    error_ = std::make_shared<ErrorType>();
    uint_ = std::make_shared<PrimitiveType>("uint");
    bigint_ = std::make_shared<PrimitiveType>("bigint");

    primitives_["int"] = int_;
    primitives_["long"] = long_;
    primitives_["short"] = short_;
    primitives_["double"] = double_;
    primitives_["float"] = float_;
    primitives_["char"] = char_;
    primitives_["boolean"] = boolean_;
    primitives_["string"] = string_;
    primitives_["void"] = void_;
    primitives_["null"] = null_;
    primitives_["dynamic"] = dynamic_;
    primitives_["uint"] = uint_;
    primitives_["bigint"] = bigint_;
}

TypeRef TypeRegistry::resolveTypeName(const std::string& name) const {
    auto it = primitives_.find(name);
    if (it != primitives_.end()) return it->second;

    auto uit = userTypes_.find(name);
    if (uit != userTypes_.end()) return uit->second;

    return nullptr;
}

TypeRef TypeRegistry::makeArray(TypeRef elem) {
    return std::make_shared<ArrayType>(std::move(elem));
}

TypeRef TypeRegistry::makeMap(TypeRef key, TypeRef val) {
    return std::make_shared<MapType>(std::move(key), std::move(val));
}

TypeRef TypeRegistry::makeSet(TypeRef elem) {
    return std::make_shared<SetType>(std::move(elem));
}

TypeRef TypeRegistry::makeTuple(std::vector<TypeRef> elems) {
    return std::make_shared<TupleTypeInfo>(std::move(elems));
}

TypeRef TypeRegistry::makeNullable(TypeRef inner) {
    return std::make_shared<NullableTypeWrapper>(std::move(inner));
}

TypeRef TypeRegistry::makeFunction(std::vector<TypeRef> params, TypeRef ret, bool async) {
    return std::make_shared<FunctionTypeInfo>(std::move(params), std::move(ret), async);
}

void TypeRegistry::registerClass(const std::string& name, TypeRef type) {
    userTypes_[name] = std::move(type);
}

void TypeRegistry::registerInterface(const std::string& name, TypeRef type) {
    userTypes_[name] = std::move(type);
}

TypeRef TypeRegistry::lookupUserType(const std::string& name) const {
    auto it = userTypes_.find(name);
    if (it != userTypes_.end()) return it->second;
    return nullptr;
}

bool TypeRegistry::isAssignable(const TypeRef& from, const TypeRef& to) const {
    if (!from || !to) return true; // unknown types pass
    if (from->isDynamic() || to->isDynamic()) return true;
    if (from->isError() || to->isError()) return true;
    if (from->isNull() && to->isNullable()) return true;
    return from->isAssignableTo(to.get());
}

bool TypeRegistry::canImplicitCoerce(const TypeRef& from, const TypeRef& to) const {
    if (!from || !to) return false;
    if (from->equals(to.get())) return true;

    // Numeric widening: int -> long, int -> double, float -> double, short -> int
    if (from->isIntegral() && to->isFloating()) return true;
    if (from->name == "int" && (to->name == "long" || to->name == "double")) return true;
    if (from->name == "short" && (to->name == "int" || to->name == "long" || to->name == "double")) return true;
    if (from->name == "float" && to->name == "double") return true;
    if (from->name == "double" && to->name == "float") return true;  // narrowing for literals
    if (from->name == "char" && to->name == "int") return true;

    // null -> T?
    if (from->isNull() && to->isNullable()) return true;

    // null -> any class/interface type (reference types are nullable by default in Gard)
    if (from->isNull() && (to->category == TypeCategory::Class || to->category == TypeCategory::Interface)) return true;

    // null -> string (strings are reference types in Gard)
    if (from->isNull() && to->isString()) return true;

    // T -> T?
    if (to->isNullable()) {
        auto* nullable = dynamic_cast<const NullableTypeWrapper*>(to.get());
        if (nullable && from->isAssignableTo(nullable->inner.get())) return true;
    }

    return false;
}

TypeRef TypeRegistry::commonType(const TypeRef& a, const TypeRef& b) const {
    if (!a || !b) return unknown_;
    if (a->equals(b.get())) return a;

    // Numeric promotion
    if (a->isNumeric() && b->isNumeric()) {
        if (a->name == "double" || b->name == "double") return double_;
        if (a->name == "float" || b->name == "float") return float_;
        if (a->name == "long" || b->name == "long") return long_;
        return int_;
    }

    // If one is nullable, result is nullable
    if (a->isNullable() || b->isNullable()) {
        auto inner = a->isNullable() ? a : b;
        return inner;
    }

    return dynamic_;
}

TypeRef TypeRegistry::binaryOpResult(const std::string& op, const TypeRef& left, const TypeRef& right) const {
    if (!left || !right) return unknown_;
    if (left->isDynamic() || right->isDynamic()) return dynamic_;

    // Arithmetic: + - * / %
    if (op == "+" || op == "-" || op == "*" || op == "/" || op == "%") {
        // String concatenation
        if (op == "+" && (left->isString() || right->isString())) {
            return string_;
        }
        // Numeric arithmetic
        if (left->isNumeric() && right->isNumeric()) {
            return commonType(left, right);
        }
    }

    // Comparison: == != < > <= >=
    if (op == "==" || op == "!=" || op == "<" || op == ">" || op == "<=" || op == ">=") {
        return boolean_;
    }

    // Logical: && ||
    if (op == "&&" || op == "||") {
        return boolean_;
    }

    // Bitwise: & | ^ << >> >>>
    if (op == "&" || op == "|" || op == "^" || op == "<<" || op == ">>" || op == ">>>") {
        if (left->isIntegral() && right->isIntegral()) {
            return commonType(left, right);
        }
    }

    // Null coalesce: ??
    if (op == "??") {
        return right; // result is the non-null type
    }

    return unknown_;
}

TypeRef TypeRegistry::unaryOpResult(const std::string& op, const TypeRef& operand) const {
    if (!operand) return unknown_;
    if (operand->isDynamic()) return dynamic_;

    if (op == "-") {
        if (operand->isNumeric()) return operand;
    }
    if (op == "!") {
        return boolean_;
    }
    if (op == "~") {
        if (operand->isIntegral()) return operand;
    }
    if (op == "++" || op == "--") {
        if (operand->isNumeric()) return operand;
    }

    return unknown_;
}

} // namespace gard
