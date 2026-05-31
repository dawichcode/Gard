#pragma once
// GARDVM Tier 2 — Shape Table (Compile-Time Field Layout)
// Shapes are resolved at compile time from class declarations.
// No dynamic shape transitions — Gard is statically typed.

#include "value.h"
#include <cstdint>
#include <cstring>
#include <vector>
#include <string>
#include <unordered_map>

namespace gardvm {

// ============================================================================
// Field Descriptor — One field in a shape
// ============================================================================

struct FieldDescriptor {
    uint16_t slotIndex;     // index into fields[] array
    uint16_t nameIndex;     // index into constant pool (field name)
    uint8_t  expectedType;  // 0=any, 1=int, 2=double, 3=bool, 4=string, 5=object
    uint8_t  flags;         // 0x01=readonly, 0x02=private, 0x04=has_default
    uint16_t _pad;
};

// ============================================================================
// Shape — Describes the layout of a class
// ============================================================================

struct Shape {
    uint32_t id;            // unique shape ID
    uint32_t fieldCount;    // number of fields
    uint32_t totalSize;     // sizeof(ObjHeader) + fieldCount * sizeof(GValue)
    uint32_t nameIndex;     // class name in constant pool
    uint32_t parentShapeId; // parent class shape (0xFFFFFFFF = none)
    uint32_t methodCount;   // number of methods in vtable
    uint32_t vtableOffset;  // offset into global vtable array

    std::vector<FieldDescriptor> fields;
    std::string className;

    // Look up field slot by name index. Returns -1 if not found.
    int32_t findField(uint16_t nameIdx) const {
        for (const auto& f : fields) {
            if (f.nameIndex == nameIdx) return (int32_t)f.slotIndex;
        }
        return -1;
    }

    // Look up field slot by name string (slower, for debug/reflection)
    int32_t findFieldByName(const std::string& name, const std::vector<std::string>& names) const {
        for (const auto& f : fields) {
            if (f.nameIndex < names.size() && names[f.nameIndex] == name) {
                return (int32_t)f.slotIndex;
            }
        }
        return -1;
    }
};

// ============================================================================
// ShapeTable — Registry of all shapes in the program
// ============================================================================

class ShapeTable {
public:
    ShapeTable() {
        // Reserve shape 0 as "unknown/dynamic" with 32 fields
        Shape unknown;
        unknown.id = 0;
        unknown.fieldCount = 32;
        unknown.totalSize = (uint32_t)(sizeof(uint64_t) + 32 * sizeof(GValue));
        unknown.nameIndex = 0;
        unknown.parentShapeId = 0xFFFFFFFF;
        unknown.methodCount = 0;
        unknown.vtableOffset = 0;
        unknown.className = "<unknown>";
        shapes_.push_back(unknown);
    }

    // Create a new shape for a class. Returns the shape ID.
    uint32_t createShape(const std::string& className, uint32_t fieldCount) {
        uint32_t id = (uint32_t)shapes_.size();
        Shape shape;
        shape.id = id;
        shape.fieldCount = fieldCount;
        shape.totalSize = (uint32_t)(sizeof(uint64_t) + fieldCount * sizeof(GValue)); // header + fields
        shape.nameIndex = 0;
        shape.parentShapeId = 0xFFFFFFFF;
        shape.methodCount = 0;
        shape.vtableOffset = 0;
        shape.className = className;

        // Create field descriptors with sequential slots
        for (uint32_t i = 0; i < fieldCount; i++) {
            FieldDescriptor fd;
            fd.slotIndex = (uint16_t)i;
            fd.nameIndex = 0; // will be set by caller
            fd.expectedType = 0;
            fd.flags = 0;
            fd._pad = 0;
            shape.fields.push_back(fd);
        }

        shapes_.push_back(shape);
        nameToId_[className] = id;
        return id;
    }

    // Create shape with named fields
    uint32_t createShapeWithFields(const std::string& className,
                                    const std::vector<std::pair<uint16_t, std::string>>& fieldNames) {
        uint32_t fieldCount = (uint32_t)fieldNames.size();
        uint32_t id = (uint32_t)shapes_.size();

        Shape shape;
        shape.id = id;
        shape.fieldCount = fieldCount;
        shape.totalSize = (uint32_t)(sizeof(uint64_t) + fieldCount * sizeof(GValue));
        shape.nameIndex = 0;
        shape.parentShapeId = 0xFFFFFFFF;
        shape.methodCount = 0;
        shape.vtableOffset = 0;
        shape.className = className;

        for (uint32_t i = 0; i < fieldCount; i++) {
            FieldDescriptor fd;
            fd.slotIndex = (uint16_t)i;
            fd.nameIndex = fieldNames[i].first;
            fd.expectedType = 0;
            fd.flags = 0;
            fd._pad = 0;
            shape.fields.push_back(fd);
        }

        shapes_.push_back(shape);
        nameToId_[className] = id;
        return id;
    }

    // Set parent shape (for inheritance)
    void setParent(uint32_t shapeId, uint32_t parentId) {
        if (shapeId < shapes_.size()) {
            shapes_[shapeId].parentShapeId = parentId;
        }
    }

    // Add a field to an existing shape (used during dynamic shape building)
    void addField(uint32_t shapeId, uint16_t nameIndex) {
        if (shapeId >= shapes_.size()) return;
        Shape& shape = shapes_[shapeId];
        FieldDescriptor fd;
        fd.slotIndex = (uint16_t)shape.fields.size();
        fd.nameIndex = nameIndex;
        fd.expectedType = 0;
        fd.flags = 0;
        fd._pad = 0;
        shape.fields.push_back(fd);
        shape.fieldCount = (uint32_t)shape.fields.size();
        shape.totalSize = (uint32_t)(sizeof(uint64_t) + shape.fieldCount * sizeof(GValue));
    }

    // Get shape by ID
    const Shape* get(uint32_t id) const {
        if (id < shapes_.size()) return &shapes_[id];
        return nullptr;
    }

    Shape* getMut(uint32_t id) {
        if (id < shapes_.size()) return &shapes_[id];
        return nullptr;
    }

    // Get shape by class name
    const Shape* getByName(const std::string& name) const {
        auto it = nameToId_.find(name);
        if (it != nameToId_.end()) return &shapes_[it->second];
        return nullptr;
    }

    uint32_t getIdByName(const std::string& name) const {
        auto it = nameToId_.find(name);
        if (it != nameToId_.end()) return it->second;
        return 0; // unknown shape
    }

    // Find field in shape (with inheritance chain walk)
    int32_t resolveField(uint32_t shapeId, uint16_t nameIndex) const {
        const Shape* shape = get(shapeId);
        while (shape) {
            int32_t slot = shape->findField(nameIndex);
            if (slot >= 0) return slot;
            // Walk up inheritance chain
            if (shape->parentShapeId == 0xFFFFFFFF) break;
            shape = get(shape->parentShapeId);
        }
        return -1;
    }

    size_t count() const { return shapes_.size(); }

private:
    std::vector<Shape> shapes_;
    std::unordered_map<std::string, uint32_t> nameToId_;
};

} // namespace gardvm
