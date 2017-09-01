/*
 * Copyright 2017 WebAssembly Community Group participants
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
// Helper functions for supporting potentially-trapping float operations.
//

#ifndef wasm_float_clamp_h
#define wasm_float_clamp_h

#include "asm_v_wasm.h"
#include "mixed_arena.h"
#include "pass.h"
#include "wasm.h"
#include "wasm-builder.h"
#include "wasm-printing.h"
#include "wasm-type.h"
#include "asmjs/shared-constants.h"
#include "support/name.h"

namespace wasm {

enum class FloatTrapMode {
  Allow,
  Clamp,
  JS
};

struct FloatTrapContext {
  FloatTrapMode trapMode;
  Module& wasm;
  MixedArena& allocator;
  Builder builder;

  FloatTrapContext(FloatTrapMode trapMode, Module& wasm)
    : trapMode(trapMode),
      wasm(wasm),
      allocator(wasm.allocator),
      builder(wasm) {}
};

Name I64S_REM("i64s-rem"),
     I64U_REM("i64u-rem"),
     I64S_DIV("i64s-div"),
     I64U_DIV("i64u-div");

static std::map<Name, Function*> addedFunctions;

void ensureBinaryFunc(FloatTrapContext& context,
                      Name name, BinaryOp op, WasmType type) {
  if (addedFunctions.count(name) != 0) {
    return;
  }

  bool is32Bit = type == i32;
  Builder& builder = context.builder;
  Expression* result = builder.makeBinary(op,
    builder.makeGetLocal(0, type),
    builder.makeGetLocal(1, type)
  );
  BinaryOp divSIntOp = is32Bit ? DivSInt32 : DivSInt64;
  BinaryOp eqOp =      is32Bit ? EqInt32   : EqInt64;
  UnaryOp eqZOp =      is32Bit ? EqZInt32  : EqZInt64;
  Literal minLit = is32Bit ? Literal(std::numeric_limits<int32_t>::min())
                           : Literal(std::numeric_limits<int64_t>::min());
  Literal negLit = is32Bit ? Literal(int32_t(-1)) : Literal(int64_t(-1));
  Literal zeroLit = is32Bit ? Literal(int32_t(0)) : Literal(int64_t(0));
  if (op == divSIntOp) {
    // guard against signed division overflow
    result = builder.makeIf(
      builder.makeBinary(AndInt32,
        builder.makeBinary(eqOp,
          builder.makeGetLocal(0, type),
          builder.makeConst(minLit)
        ),
        builder.makeBinary(eqOp,
          builder.makeGetLocal(1, type),
          builder.makeConst(negLit)
        )
      ),
      builder.makeConst(zeroLit),
      result
    );
  }
  auto func = new Function;
  func->name = name;
  func->params.push_back(type);
  func->params.push_back(type);
  func->result = type;
  func->body = builder.makeIf(
    builder.makeUnary(eqZOp,
      builder.makeGetLocal(1, type)
    ),
    builder.makeConst(zeroLit),
    result
  );
  addedFunctions[name] = func;
}

// Some binary opts might trap, so emit them safely if necessary
Expression* makeTrappingBinary(Binary* curr, FloatTrapContext& context) {
  if (context.trapMode == FloatTrapMode::Allow) {
    return curr;
  }
  // the wasm operation might trap if done over 0, so generate a safe call
  WasmType type = curr->type;
  auto *call = context.allocator.alloc<Call>();
  switch (curr->op) {
    case BinaryOp::RemSInt32: call->target = I32S_REM; break;
    case BinaryOp::RemUInt32: call->target = I32U_REM; break;
    case BinaryOp::DivSInt32: call->target = I32S_DIV; break;
    case BinaryOp::DivUInt32: call->target = I32U_DIV; break;
    case BinaryOp::RemSInt64: call->target = I64S_REM; break;
    case BinaryOp::RemUInt64: call->target = I64U_REM; break;
    case BinaryOp::DivSInt64: call->target = I64S_DIV; break;
    case BinaryOp::DivUInt64: call->target = I64U_DIV; break;
    default:
      return curr;
  }
  call->operands.push_back(curr->left);
  call->operands.push_back(curr->right);
  call->type = type;
  ensureBinaryFunc(context, call->target, curr->op, type);
  return call;
}

// Some conversions might trap, so emit them safely if necessary
Expression* makeTrappingFloatToInt32(Unary* curr, FloatTrapContext context) {
  if (context.trapMode == FloatTrapMode::Allow) {
    return curr;
  }
  // WebAssembly traps on float-to-int overflows, but asm.js wouldn't, so we must do something
  // We can handle this in one of two ways: clamping, which is fast, or JS, which
  // is precisely like JS but in order to do that we do a slow ffi
  if (context.trapMode == FloatTrapMode::JS) {
    // WebAssembly traps on float-to-int overflows, but asm.js wouldn't, so we must emulate that
    CallImport *ret = context.allocator.alloc<CallImport>();
    ret->target = F64_TO_INT;
    ret->operands.push_back(ensureDouble(curr->value, context.allocator));
    ret->type = i32;
    static bool addedImport = false;
    if (!addedImport) {
      addedImport = true;
      auto import = new Import; // f64-to-int = asm2wasm.f64-to-int;
      import->name = F64_TO_INT;
      import->module = ASM2WASM;
      import->base = F64_TO_INT;
      import->functionType = ensureFunctionType("id", &context.wasm)->name;
      import->kind = ExternalKind::Function;
      context.wasm.addImport(import);
    }
    return ret;
  }
  assert(context.trapMode == FloatTrapMode::Clamp);
  WasmType type = curr->value->type;
  bool isF64 = type == f64;
  Name name = isF64 ? F64_TO_INT : F32_TO_INT;
  UnaryOp truncOp = isF64 ? TruncSFloat64ToInt32 : TruncSFloat32ToInt32;
  BinaryOp leOp = isF64 ? LeFloat64 : LeFloat32;
  BinaryOp geOp = isF64 ? GeFloat64 : GeFloat32;
  BinaryOp neOp = isF64 ? NeFloat64 : NeFloat32;
  int32_t iMin = std::numeric_limits<int32_t>::min();
  int32_t iMax = std::numeric_limits<int32_t>::max();
  Literal fMin = isF64 ? Literal(double(iMin) - 1)
                       : Literal( float(iMin) - 1);
  Literal fMax = isF64 ? Literal(double(iMax) + 1)
                       : Literal( float(iMax) + 1);
  Call *ret = context.allocator.alloc<Call>();
  ret->target = name;
  ret->operands.push_back(curr->value);
  ret->type = i32;
  static std::set<Name> local_addedFunctions;
  if (local_addedFunctions.count(name) == 0) {
    Builder& builder = context.builder;
    local_addedFunctions.insert(name);
    auto func = new Function;
    func->name = name;
    func->params.push_back(type);
    func->result = i32;
    func->body = builder.makeUnary(truncOp,
      builder.makeGetLocal(0, type)
    );
    // too small XXX this is different than asm.js, which does frem. here we clamp, which is much simpler/faster, and similar to native builds
    func->body = builder.makeIf(
      builder.makeBinary(leOp,
        builder.makeGetLocal(0, type),
        builder.makeConst(fMin)
      ),
      builder.makeConst(Literal(iMin)),
      func->body
    );
    // too big XXX see above
    func->body = builder.makeIf(
      builder.makeBinary(geOp,
        builder.makeGetLocal(0, type),
        builder.makeConst(fMax)
      ),
      // NB: min here as well. anything out of range => to the min
      builder.makeConst(Literal(iMin)),
      func->body
    );
    // nan
    func->body = builder.makeIf(
      builder.makeBinary(neOp,
        builder.makeGetLocal(0, type),
        builder.makeGetLocal(0, type)
      ),
      // NB: min here as well. anything invalid => to the min
      builder.makeConst(Literal(iMin)),
      func->body
    );
    context.wasm.addFunction(func);
  }
  return ret;
}

Expression* makeTrappingFloatToInt64(bool isSigned, Expression* value, FloatTrapContext context) {
  if (context.trapMode == FloatTrapMode::Allow) {
    auto ret = context.allocator.alloc<Unary>();
    ret->value = value;
    bool isF64 = ret->value->type == f64;
    UnaryOp op;
    if (isSigned && isF64) {
      op = TruncSFloat64ToInt64;
    } else if (isSigned && !isF64) {
      op = TruncSFloat32ToInt64;
    } else if (!isSigned && isF64) {
      op = TruncUFloat64ToInt64;
    } else if (!isSigned && !isF64) {
      op = TruncUFloat32ToInt64;
    } else {
      WASM_UNREACHABLE();
    }
    ret->op = op;
    ret->type = WasmType::i64;
    return ret;
  }
  // WebAssembly traps on float-to-int overflows, but asm.js wouldn't, so we must do something
  // First, normalize input to f64
  auto input = ensureDouble(value, context.allocator);
  // There is no "JS" way to handle this, as no i64s in JS, so always clamp if we don't allow traps
  Call *ret = context.allocator.alloc<Call>();
  ret->target = F64_TO_INT64;
  ret->operands.push_back(input);
  ret->type = i64;
  static bool added = false;
  if (!added) {
    Builder& builder = context.builder;
    added = true;
    auto func = new Function;
    func->name = ret->target;
    func->params.push_back(f64);
    func->result = i64;
    func->body = builder.makeUnary(TruncSFloat64ToInt64,
      builder.makeGetLocal(0, f64)
    );
    // too small
    func->body = builder.makeIf(
      builder.makeBinary(LeFloat64,
        builder.makeGetLocal(0, f64),
        builder.makeConst(Literal(double(std::numeric_limits<int64_t>::min()) - 1))
      ),
      builder.makeConst(Literal(int64_t(std::numeric_limits<int64_t>::min()))),
      func->body
    );
    // too big
    func->body = builder.makeIf(
      builder.makeBinary(GeFloat64,
        builder.makeGetLocal(0, f64),
        builder.makeConst(Literal(double(std::numeric_limits<int64_t>::max()) + 1))
      ),
      builder.makeConst(Literal(int64_t(std::numeric_limits<int64_t>::min()))), // NB: min here as well. anything out of range => to the min
      func->body
    );
    // nan
    func->body = builder.makeIf(
      builder.makeBinary(NeFloat64,
        builder.makeGetLocal(0, f64),
        builder.makeGetLocal(0, f64)
      ),
      builder.makeConst(Literal(int64_t(std::numeric_limits<int64_t>::min()))), // NB: min here as well. anything invalid => to the min
      func->body
    );
    context.wasm.addFunction(func);
  }
  return ret;
}

void addAddedFunctions(Module& wasm) {
  for (const auto& pair : addedFunctions) {
    wasm.addFunction(pair.second);
  }
  addedFunctions.clear();
}

struct BinaryenTrapMode : public WalkerPass<PostWalker<BinaryenTrapMode>> {
  bool isFunctionParallel() override { return false; }

  FloatTrapMode mode;
  BinaryenTrapMode(FloatTrapMode mode) : mode(mode) {
    name = "binaryen-trap-mode";
  }

  Pass* create() override { return new BinaryenTrapMode(mode); }

  void visitUnary(Unary* curr) {
    FloatTrapContext context(mode, *getModule());
    switch (curr->op) {
    case UnaryOp::TruncSFloat32ToInt32:
    case UnaryOp::TruncSFloat64ToInt32:
    case UnaryOp::TruncUFloat32ToInt32:
    case UnaryOp::TruncUFloat64ToInt32:
      replaceCurrent(makeTrappingFloatToInt32(curr, context));
      break;

    case UnaryOp::TruncSFloat32ToInt64:
    case UnaryOp::TruncSFloat64ToInt64:
      replaceCurrent(makeTrappingFloatToInt64(true, curr->value, context));
      break;

    case UnaryOp::TruncUFloat32ToInt64:
    case UnaryOp::TruncUFloat64ToInt64:
      replaceCurrent(makeTrappingFloatToInt64(false, curr->value, context));
      break;

    default:
      break;
    }
  }

  void visitBinary(Binary* curr) {
    FloatTrapContext context(mode, *getModule());
    replaceCurrent(makeTrappingBinary(curr, context));
  }

  void visitModule(Module* curr) {
    addAddedFunctions(*curr);
  }
};

} // namespace wasm

#endif // wasm_float_clamp_h