/*
 * Copyright 2021 WebAssembly Community Group participants
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

//
// Converts vtables - structs of function references - to use indexes. That is,
// this replaces function reference fields with i32 fields. Specifically,
//
//  (struct (field (ref $functype1)) (field (ref $functype2))
// =>
//  (struct (field (ref i32))        (field (ref i32))
//
// This also creates a table for each field and populates it with the possible
// values. Then struct.news are altered to replace references with indexes, and
// struct.gets are altered to load from the table.
//
// Assumptions:
//  * All function reference fields are to be transformed.
//  * Such fields must be written to during creation of a vtable instance, and
//    with a constant ref.func.
//  * Vtable subtyping is allowed, but not to specialize types of the parent. If
//    that were done, we'd need to add casts to handle the table no having the
//    specialized type (it would have the subtype).
//

#include <ir/module-utils.h>
//#include "ir/subtypes.h" // Needed?
#include <pass.h>
#include <wasm.h>
#include <wasm-type.h>

using namespace std;

namespace wasm {

namespace {

using HeapTypeMap = std::unordered_map<HeapType, HeapType>;

struct VTableToIndexes : public Pass {
  void run(PassRunner* runner, Module* module) override {
    // Create the new types and get a mapping of the old ones to the new.
    auto oldToNewTypes = mapOldTypesToNew(*module);

    // Update all the types to the new ones.
    updateTypes(runner, *module, oldToNewTypes);
  }

  HeapTypeMap mapOldTypesToNew(Module& wasm) {
    // Collect all the types.
    std::vector<HeapType> types;
    std::unordered_map<HeapType, Index> typeIndices;
    ModuleUtils::collectHeapTypes(wasm, types, typeIndices);

    // We will need to map types to their indexes.
    std::unordered_map<HeapType, Index> typeToIndex;
    for (Index i = 0; i < types.size(); i++) {
      typeToIndex[types[i]] = i;
    }

    TypeBuilder typeBuilder(types.size());

    // Map an old type to a new type. This is called on the contents of the
    // temporary heap types, so it basically just needs to map to other temp
    // heap types.
    std::function<Type (Type)> getNewType = [&](Type type) {
      if (type.isBasic()) {
        return type;
      }
      if (type.isRef()) {
        return typeBuilder.getTempRefType(
          typeBuilder.getTempHeapType(typeToIndex.at(type.getHeapType())),
          type.getNullability()
        );
      }
      if (type.isRtt()) {
        auto rtt = type.getRtt();
        auto newRtt = rtt;
        newRtt.heapType = 
          typeBuilder.getTempHeapType(typeToIndex.at(type.getHeapType()));
        return typeBuilder.getTempRttType(newRtt);
      }
      if (type.isTuple()) {
        auto& tuple = type.getTuple();
        auto newTuple = tuple;
        for (auto& t : newTuple.types) {
          t = getNewType(t);
        }
        return typeBuilder.getTempTupleType(newTuple);
      }
      WASM_UNREACHABLE("bad type");
    };

    // Map an old type to a new type, for a struct field. This does the special
    // operation we are doing here, which is to replace function referencs with
    // i32s.
    auto getNewTypeForStruct = [&](Type type) -> Type {
      if (type.isFunction()) {
        // This is exactly what we are looking to change!
        return Type::i32;
      }
      return getNewType(type);
    };

    // Create the temporary heap types.
    for (Index i = 0; i < types.size(); i++) {
      auto type = types[i];
      if (type.isSignature()) {
        auto sig = type.getSignature();
        TypeList newParams, newResults;
        for (auto t : sig.params) {
          newParams.push_back(getNewType(t));
        }
        for (auto t : sig.results) {
          newResults.push_back(getNewType(t));
        }
        typeBuilder.setHeapType(i, Signature(typeBuilder.getTempTupleType(newParams), typeBuilder.getTempTupleType(newResults)));
      } else if (type.isStruct()) {
        auto struct_ = type.getStruct();
        // Start with a copy to get mutability/packing/etc.
        auto newStruct = struct_;
        for (auto& field : newStruct.fields) {
          field.type = getNewTypeForStruct(field.type);
        }
        typeBuilder.setHeapType(i, newStruct);
      } else if (type.isArray()) {
        auto array = type.getArray();
        // Start with a copy to get mutability/packing/etc.
        auto newArray = array;
        newArray.element.type = getNewType(newArray.element.type);
        typeBuilder.setHeapType(i, newArray);
      } else {
        WASM_UNREACHABLE("bad type");
      }
    }
    auto newTypes = typeBuilder.build();

    // Return a mapping of the old types to the new.
    HeapTypeMap oldToNewTypes;
    for (Index i = 0; i < types.size(); i++) {
      oldToNewTypes[types[i]] = newTypes[i];
    }
    return oldToNewTypes;
  }

  void updateTypes(PassRunner* runner, Module& wasm, HeapTypeMap& oldToNewTypes) {
    struct CodeUpdater
      : public WalkerPass<PostWalker<CodeUpdater, UnifiedExpressionVisitor<CodeUpdater>>> {
      bool isFunctionParallel() override { return true; }

      HeapTypeMap& oldToNewTypes;

      CodeUpdater(HeapTypeMap& oldToNewTypes) : oldToNewTypes(oldToNewTypes) {}

      CodeUpdater* create() override {
        return new CodeUpdater(oldToNewTypes);
      }

      Type update(Type type) {
        if (type.isRef()) {
          return Type(update(type.getHeapType()), type.getNullability());
        }
        if (type.isRtt()) {
          return Type(Rtt(type.getRtt().depth, update(type.getHeapType())));
        }
        return type;
      }

      HeapType update(HeapType type) {
        if (type.isBasic()) {
          return type;
        }
        if (type.isFunction() || type.isData()) {
          return oldToNewTypes.at(type);
        }
        return type;
      }

      Signature update(Signature sig) {
        return Signature(update(sig.params), update(sig.results));
      }

      void visitExpression(Expression* curr) {
        // Update the type to the new one.
        curr->type = update(curr->type);

        // If this is a struct.get, then we also update function reference types
        // to i32.
        if (auto* get = curr->dynCast<StructGet>()) {
          if (get->type.isFunction()) {
            get->type = Type::i32;
          }
        }

        // Update any other type fields as well.

#define DELEGATE_ID curr->_id

#define DELEGATE_START(id)                                                     \
  auto* cast = curr->cast<id>();                                               \
  WASM_UNUSED(cast);

#define DELEGATE_GET_FIELD(id, name) cast->name

#define DELEGATE_FIELD_TYPE(id, name) \
  cast->name = update(cast->name);

#define DELEGATE_FIELD_HEAPTYPE(id, name) \
  cast->name = update(cast->name);

#define DELEGATE_FIELD_SIGNATURE(id, name) \
  cast->name = update(cast->name);

#define DELEGATE_FIELD_CHILD(id, name)
#define DELEGATE_FIELD_OPTIONAL_CHILD(id, name)
#define DELEGATE_FIELD_INT(id, name)
#define DELEGATE_FIELD_INT_ARRAY(id, name)
#define DELEGATE_FIELD_LITERAL(id, name)
#define DELEGATE_FIELD_NAME(id, name)
#define DELEGATE_FIELD_NAME_VECTOR(id, name)
#define DELEGATE_FIELD_SCOPE_NAME_DEF(id, name)
#define DELEGATE_FIELD_SCOPE_NAME_USE(id, name)
#define DELEGATE_FIELD_SCOPE_NAME_USE_VECTOR(id, name)
#define DELEGATE_FIELD_ADDRESS(id, name)

#include "wasm-delegations-fields.def"
      }
    };

    CodeUpdater updater(oldToNewTypes);
    updater.run(runner, &wasm);
    updater.walkModuleCode(&wasm);

    // Update global locations that refer to types.
    for (auto& table : wasm.tables) {
      table->type = updater.update(table->type);
    }
    for (auto& elementSegment : wasm.elementSegments) {
      elementSegment->type = updater.update(elementSegment->type);
    }
    for (auto& global : wasm.globals) {
      global->type = updater.update(global->type);
    }
    for (auto& func : wasm.functions) {
      func->type = updater.update(func->type);
    }

    for (auto& kv : oldToNewTypes) {
      auto old = kv.first;
      auto new_ = kv.second;
      if (wasm.typeNames.count(old)) {
        wasm.typeNames[new_] = wasm.typeNames[old];
      }
    }
  }
};

    //SubTypes subTypes(*module);

} // anonymous namespace

Pass* createVTableToIndexesPass() { return new VTableToIndexes(); }

} // namespace wasm