#pragma once

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <optional>

namespace gard {

// Forward declarations
struct Type;
using TypeRef = std::shared_ptr<Type>;

// --- Type kinds ---

enum class TypeCategory {
    Primitive,
    Collection,
    Tuple,
    Nullable,
    Function,
    Class,
    Interface,
    Generic,
    GenericParam,
    Void,
    Null,
    Dynamic,
    Unknown,    // for unresolved types
    Error,      // for error recovery
};

// --- Type representation ---

struct Type {
    TypeCategory category;
    std::string name;

    virtual ~Type() = default;
    virtual std::string toString() const { return name; }
    virtual bool equals(const Type* other) const;
    virtual bool isAssignableTo(const Type* target) const;

    bool isPrimitive() const { return category == TypeCategory::Primitive; }
    bool isNumeric() const;
    bool isIntegral() const;
    bool isFloating() const;
    bool isString() const { return category == TypeCategory::Primitive && name == "string"; }
    bool isBoolean() const { return category == TypeCategory::Primitive && name == "boolean"; }
    bool isVoid() const { return category == TypeCategory::Void; }
    bool isNull() const { return category == TypeCategory::Null; }
    bool isDynamic() const { return category == TypeCategory::Dynamic; }
    bool isNullable() const { return category == TypeCategory::Nullable; }
    bool isError() const { return category == TypeCategory::Error; }
    bool isUnknown() const { return category == TypeCategory::Unknown; }

protected:
    Type(TypeCategory cat, const std::string& name) : category(cat), name(name) {}
};

// --- Primitive types ---

struct PrimitiveType : Type {
    PrimitiveType(const std::string& name) : Type(TypeCategory::Primitive, name) {}
};

// --- Void type ---

struct VoidType : Type {
    VoidType() : Type(TypeCategory::Void, "void") {}
};

// --- Null type ---

struct NullType : Type {
    NullType() : Type(TypeCategory::Null, "null") {}
};

// --- Dynamic type ---

struct DynamicType : Type {
    DynamicType() : Type(TypeCategory::Dynamic, "dynamic") {}
    bool isAssignableTo(const Type*) const override { return true; }
};

// --- Unknown type (for error recovery) ---

struct UnknownType : Type {
    UnknownType() : Type(TypeCategory::Unknown, "<unknown>") {}
    bool isAssignableTo(const Type*) const override { return true; }
};

// --- Error type (suppresses cascading errors) ---

struct ErrorType : Type {
    ErrorType() : Type(TypeCategory::Error, "<error>") {}
    bool isAssignableTo(const Type*) const override { return true; }
};

// --- Collection types ---

struct ArrayType : Type {
    TypeRef elementType;

    ArrayType(TypeRef elem) : Type(TypeCategory::Collection, "array"), elementType(std::move(elem)) {}
    std::string toString() const override { return "array<" + elementType->toString() + ">"; }
    bool equals(const Type* other) const override;
    bool isAssignableTo(const Type* target) const override;
};

struct MapType : Type {
    TypeRef keyType;
    TypeRef valueType;

    MapType(TypeRef key, TypeRef val)
        : Type(TypeCategory::Collection, "map"), keyType(std::move(key)), valueType(std::move(val)) {}
    std::string toString() const override {
        return "map<" + keyType->toString() + ", " + valueType->toString() + ">";
    }
    bool equals(const Type* other) const override;
    bool isAssignableTo(const Type* target) const override;
};

struct SetType : Type {
    TypeRef elementType;

    SetType(TypeRef elem) : Type(TypeCategory::Collection, "set"), elementType(std::move(elem)) {}
    std::string toString() const override { return "set<" + elementType->toString() + ">"; }
    bool equals(const Type* other) const override;
    bool isAssignableTo(const Type* target) const override;
};

// --- Tuple type ---

struct TupleTypeInfo : Type {
    std::vector<TypeRef> elements;

    TupleTypeInfo(std::vector<TypeRef> elems)
        : Type(TypeCategory::Tuple, "tuple"), elements(std::move(elems)) {}
    std::string toString() const override;
    bool equals(const Type* other) const override;
    bool isAssignableTo(const Type* target) const override;
};

// --- Nullable type ---

struct NullableTypeWrapper : Type {
    TypeRef inner;

    NullableTypeWrapper(TypeRef inner)
        : Type(TypeCategory::Nullable, inner->name + "?"), inner(std::move(inner)) {}
    std::string toString() const override { return inner->toString() + "?"; }
    bool equals(const Type* other) const override;
    bool isAssignableTo(const Type* target) const override;
};

// --- Function type ---

struct FunctionTypeInfo : Type {
    std::vector<TypeRef> paramTypes;
    TypeRef returnType;
    bool isAsync = false;

    FunctionTypeInfo(std::vector<TypeRef> params, TypeRef ret, bool async = false)
        : Type(TypeCategory::Function, "function"),
          paramTypes(std::move(params)), returnType(std::move(ret)), isAsync(async) {}
    std::string toString() const override;
    bool equals(const Type* other) const override;
    bool isAssignableTo(const Type* target) const override;
};

// --- Class type ---

struct ClassType : Type {
    std::string className;
    TypeRef baseClass;                          // parent class
    std::vector<std::string> interfaces;        // implemented interfaces
    std::unordered_map<std::string, TypeRef> fields;
    std::unordered_map<std::string, TypeRef> methods; // method return types
    std::vector<std::string> genericParams;
    bool isAbstract = false;

    ClassType(const std::string& name)
        : Type(TypeCategory::Class, name), className(name) {}
    std::string toString() const override { return className; }
    bool equals(const Type* other) const override;
    bool isAssignableTo(const Type* target) const override;
};

// --- Interface type ---

struct InterfaceType : Type {
    std::string interfaceName;
    std::unordered_map<std::string, TypeRef> methods;

    InterfaceType(const std::string& name)
        : Type(TypeCategory::Interface, name), interfaceName(name) {}
    std::string toString() const override { return interfaceName; }
    bool equals(const Type* other) const override;
    bool isAssignableTo(const Type* target) const override;
};

// --- Generic instantiation ---

struct GenericInstType : Type {
    std::string baseName;
    std::vector<TypeRef> typeArgs;

    GenericInstType(const std::string& base, std::vector<TypeRef> args)
        : Type(TypeCategory::Generic, base), baseName(base), typeArgs(std::move(args)) {}
    std::string toString() const override;
    bool equals(const Type* other) const override;
    bool isAssignableTo(const Type* target) const override;
};

// --- Generic type parameter ---

struct GenericParamType : Type {
    std::string paramName;
    TypeRef constraint; // primary bound (first constraint)
    std::vector<TypeRef> constraints; // all bounds: <T: A & B & C>
    bool reified = false; // type info available at runtime

    GenericParamType(const std::string& name, TypeRef constraint = nullptr)
        : Type(TypeCategory::GenericParam, name), paramName(name), constraint(std::move(constraint)) {}
    std::string toString() const override { return paramName; }
    bool equals(const Type* other) const override;
    bool isAssignableTo(const Type* target) const override;
};

// --- Type Registry (singleton-like for built-in types) ---

class TypeRegistry {
public:
    TypeRegistry();

    // Get built-in types
    TypeRef getInt() const { return int_; }
    TypeRef getLong() const { return long_; }
    TypeRef getShort() const { return short_; }
    TypeRef getDouble() const { return double_; }
    TypeRef getFloat() const { return float_; }
    TypeRef getChar() const { return char_; }
    TypeRef getBoolean() const { return boolean_; }
    TypeRef getString() const { return string_; }
    TypeRef getVoid() const { return void_; }
    TypeRef getNull() const { return null_; }
    TypeRef getDynamic() const { return dynamic_; }
    TypeRef getUnknown() const { return unknown_; }
    TypeRef getError() const { return error_; }
    TypeRef getUint() const { return uint_; }
    TypeRef getBigint() const { return bigint_; }

    // Resolve a type name to a TypeRef
    TypeRef resolveTypeName(const std::string& name) const;

    // Create collection types
    TypeRef makeArray(TypeRef elem);
    TypeRef makeMap(TypeRef key, TypeRef val);
    TypeRef makeSet(TypeRef elem);
    TypeRef makeTuple(std::vector<TypeRef> elems);
    TypeRef makeNullable(TypeRef inner);
    TypeRef makeFunction(std::vector<TypeRef> params, TypeRef ret, bool async = false);

    // Register user-defined types
    void registerClass(const std::string& name, TypeRef type);
    void registerInterface(const std::string& name, TypeRef type);
    TypeRef lookupUserType(const std::string& name) const;

    // Type compatibility
    bool isAssignable(const TypeRef& from, const TypeRef& to) const;
    bool canImplicitCoerce(const TypeRef& from, const TypeRef& to) const;
    TypeRef commonType(const TypeRef& a, const TypeRef& b) const;

    // Operator result types
    TypeRef binaryOpResult(const std::string& op, const TypeRef& left, const TypeRef& right) const;
    TypeRef unaryOpResult(const std::string& op, const TypeRef& operand) const;

private:
    TypeRef int_;
    TypeRef long_;
    TypeRef short_;
    TypeRef double_;
    TypeRef float_;
    TypeRef char_;
    TypeRef boolean_;
    TypeRef string_;
    TypeRef void_;
    TypeRef null_;
    TypeRef dynamic_;
    TypeRef unknown_;
    TypeRef error_;
    TypeRef uint_;
    TypeRef bigint_;

    std::unordered_map<std::string, TypeRef> primitives_;
    std::unordered_map<std::string, TypeRef> userTypes_;
};

} // namespace gard
