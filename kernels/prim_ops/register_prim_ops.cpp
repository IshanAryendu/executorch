/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * All rights reserved.
 *
 * This source code is licensed under the BSD-style license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <executorch/kernels/prim_ops/et_copy_index.h>
#include <executorch/kernels/prim_ops/et_view.h>
#include <executorch/runtime/core/evalue.h>
#include <executorch/runtime/kernel/kernel_includes.h>
#include <executorch/runtime/kernel/operator_registry.h>

#include <algorithm>
#include <cmath>

using torch::executor::function::et_copy_index;

namespace torch {
namespace executor {
namespace function {

namespace {

#define __ET_PRIM_OP_ERROR_IMPL(a, b, context) \
  else {                                       \
    ET_KERNEL_CHECK_MSG(                       \
        context,                               \
        false,                                 \
        InvalidType,                           \
        /* void */,                            \
        "%zu, %zu",                            \
        (size_t)a.tag,                         \
        (size_t)b.tag);                        \
  }

#define __NUMBER_ET_PRIM_OP_IMPL(operator, stack, context) \
  (void)context;                                           \
  EValue& a = *stack[0];                                   \
  EValue& b = *stack[1];                                   \
  EValue& out = *stack[2];                                 \
  if (a.isInt() && b.isInt()) {                            \
    out = EValue(a.toInt() operator b.toInt());            \
  } else if (a.isDouble() && b.isDouble()) {               \
    out = EValue(a.toDouble() operator b.toDouble());      \
  } else if (a.isInt() && b.isDouble()) {                  \
    out = EValue(a.toInt() operator b.toDouble());         \
  } else if (a.isDouble() && b.isInt()) {                  \
    out = EValue(a.toDouble() operator b.toInt());         \
  }

#define ALGEBRA_ET_PRIM_OP(operator, stack, context) \
  __NUMBER_ET_PRIM_OP_IMPL(operator, stack, context) \
  __ET_PRIM_OP_ERROR_IMPL(a, b, context)

#define BOOLEAN_ET_PRIM_OP(operator, stack, context) \
  __NUMBER_ET_PRIM_OP_IMPL(operator, stack, context) \
  else if (a.isBool() && b.isBool()) {               \
    out = EValue(a.toBool() operator b.toBool());    \
  }                                                  \
  __ET_PRIM_OP_ERROR_IMPL(a, b, context)

void floor_div_double(double a, double b, EValue& out) {
  if (b == 0) {
    out = EValue(std::signbit(a) ? -INFINITY : INFINITY);
    return;
  }
  const auto mod = std::fmod(a, b);
  auto div = (a - mod) / b;
  if ((mod != 0) && std::signbit(b) != std::signbit(mod)) {
    out = EValue(div - 1);
    return;
  }
  out = EValue(div);
}

static Kernel prim_ops[] = {
    // aten::sym_size.int(Tensor self, int dim) -> SymInt
    Kernel(
        "aten::sym_size.int",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& self = *stack[0];
          EValue& dim = *stack[1];
          EValue& out = *stack[2];
          executorch::aten::Tensor self_tensor =
              self.to<executorch::aten::Tensor>();
          int64_t dim_val = dim.to<int64_t>();
          int64_t size = self_tensor.size(dim_val);
          out = EValue(size);
        }),
    // aten::_local_scalar_dense(Tensor self) -> Scalar
    Kernel(
        "aten::_local_scalar_dense",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& self = *stack[0];
          EValue& out = *stack[1];
          executorch::aten::Tensor self_tensor =
              self.to<executorch::aten::Tensor>();
          ET_SWITCH_REAL_TYPES_AND(
              Bool,
              self_tensor.scalar_type(),
              context,
              "_local_scalar_dense",
              CTYPE,
              [&]() {
                out = EValue(Scalar(self_tensor.const_data_ptr<CTYPE>()[0]));
              });
        }),
    // aten::sym_numel(Tensor self) -> SymInt
    Kernel(
        "aten::sym_numel",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& self = *stack[0];
          EValue& out = *stack[1];
          executorch::aten::Tensor self_tensor =
              self.to<executorch::aten::Tensor>();
          int64_t numel = self_tensor.numel();
          out = EValue(numel);
        }),
    // executorch_prim::sym_max.Scalar(SymInt a, SymInt b) -> SymInt
    Kernel(
        "executorch_prim::sym_max.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& a = *stack[0];
          EValue& b = *stack[1];
          EValue& out = *stack[2];
          if (a.isInt() && b.isInt()) {
            out = EValue(std::max(a.toInt(), b.toInt()));
          } else {
            ET_KERNEL_CHECK_MSG(
                context,
                false,
                InvalidType,
                /* void */,
                "sym_max only supports int inputs, got %zu, %zu",
                (size_t)a.tag,
                (size_t)b.tag);
          }
        }),
    // executorch_prim::sym_min.Scalar(SymInt a, SymInt b) -> SymInt
    Kernel(
        "executorch_prim::sym_min.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& a = *stack[0];
          EValue& b = *stack[1];
          EValue& out = *stack[2];
          if (a.isInt() && b.isInt()) {
            out = EValue(std::min(a.toInt(), b.toInt()));
          } else {
            ET_KERNEL_CHECK_MSG(
                context,
                false,
                InvalidType,
                /* void */,
                "sym_min only supports int inputs, got %zu, %zu",
                (size_t)a.tag,
                (size_t)b.tag);
          }
        }),
    // executorch_prim::add.Scalar(Scalar, Scalar) -> Scalar
    Kernel(
        "executorch_prim::add.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          ALGEBRA_ET_PRIM_OP(+, stack, context);
        }),

    // executorch_prim::sub.Scalar(Scalar, Scalar) -> Scalar
    Kernel(
        "executorch_prim::sub.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          ALGEBRA_ET_PRIM_OP(-, stack, context);
        }),

    // executorch_prim::mul.Scalar(Scalar, Scalar) -> Scalar
    Kernel(
        "executorch_prim::mul.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          ALGEBRA_ET_PRIM_OP(*, stack, context);
        }),

    /**
     * Python's __floordiv__ operator is more complicated than just floor(a /
     * b). It aims to maintain the property: a == (a // b) * b + remainder(a, b)
     * which can otherwise fail due to rounding errors in the remainder.
     * So, instead it is calculated as: a // b = (a - remainder(a, b)) / b
     * With some additional fix-ups added to the result.
     *
     * executorch_prim::floordiv.Scalar(Scalar, Scalar) -> Scalar
     */
    Kernel(
        "executorch_prim::floordiv.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& a = *stack[0];
          EValue& b = *stack[1];
          EValue& out = *stack[2];
          if (a.isInt() && b.isInt()) {
            const int64_t quot = a.toInt() / b.toInt();
            if ((a.toInt() < 0) == (b.toInt() < 0)) {
              out = EValue(quot);
              return;
            }
            const int64_t rem = a.toInt() % b.toInt();
            out = EValue(rem ? quot - 1 : quot);
            return;
          } else if (a.isDouble() && b.isDouble()) {
            floor_div_double(a.toDouble(), b.toDouble(), out);
          } else if (a.isInt() && b.isDouble()) {
            floor_div_double(static_cast<double>(a.toInt()), b.toDouble(), out);
          } else if (a.isDouble() && b.isInt()) {
            floor_div_double(a.toDouble(), static_cast<double>(b.toInt()), out);
          } else {
            ET_KERNEL_CHECK_MSG(
                context,
                false,
                InvalidType,
                /* void */,
                "%zu, %zu",
                (size_t)a.tag,
                (size_t)b.tag);
          }
        }),

    // executorch_prim::floordiv.Scalar(Scalar, Scalar) -> Scalar
    Kernel(
        "executorch_prim::truediv.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          // can't use macro because of custom casting behavior
          (void)context;
          EValue& a = *stack[0];
          EValue& b = *stack[1];
          EValue& out = *stack[2];
          if (a.isInt() && b.isInt()) {
            out = EValue(
                static_cast<double>(a.toInt()) /
                static_cast<double>(b.toInt()));
          } else if (a.isDouble() && b.isDouble()) {
            out = EValue(a.toDouble() / b.toDouble());
          } else if (a.isInt() && b.isDouble()) {
            out = EValue(a.toInt() / b.toDouble());
          } else if (a.isDouble() && b.isInt()) {
            out = EValue(a.toDouble() / b.toInt());
          } else {
            ET_KERNEL_CHECK_MSG(
                context,
                false,
                InvalidType,
                /* void */,
                "%zu, %zu",
                (size_t)a.tag,
                (size_t)b.tag);
          }
        }),

    // executorch_prim::sym_float.Scalar(Scalar) -> Scalar
    Kernel(
        "executorch_prim::sym_float.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          // can't use macro because of custom casting behavior
          // TODO: Now that we are reliably generating conversion operators,
          // we can remove the mixed type handling for other operators
          (void)context;
          EValue& a = *stack[0];
          EValue& out = *stack[1];
          if (a.isInt()) {
            out = EValue(static_cast<double>(a.toInt()));
          } else if (a.isDouble()) {
            // TODO: This should be impossible
            out = EValue(a.toDouble());
          } else {
            ET_KERNEL_CHECK_MSG(
                context, false, InvalidType, /* void */, "%zu", (size_t)a.tag);
          }
        }),

    // executorch_prim::eq.Scalar(Scalar, Scalar) -> bool
    Kernel(
        "executorch_prim::eq.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          BOOLEAN_ET_PRIM_OP(==, stack, context);
        }),

    // executorch_prim::gt.Scalar(Scalar, Scalar) -> bool
    Kernel(
        "executorch_prim::gt.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          BOOLEAN_ET_PRIM_OP(>, stack, context);
        }),

    // executorch_prim::lt.Scalar(Scalar, Scalar) -> bool
    Kernel(
        "executorch_prim::lt.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          BOOLEAN_ET_PRIM_OP(<, stack, context);
        }),

    // executorch_prim::ge.Scalar(Scalar, Scalar) -> bool
    Kernel(
        "executorch_prim::ge.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          BOOLEAN_ET_PRIM_OP(>=, stack, context);
        }),

    // executorch_prim::le.Scalar(Scalar, Scalar) -> bool
    Kernel(
        "executorch_prim::le.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          BOOLEAN_ET_PRIM_OP(<=, stack, context);
        }),
    // executorch_prim::neg.Scalar(Scalar) -> Scalar
    Kernel(
        "executorch_prim::neg.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& a = *stack[0];
          EValue& out = *stack[1];
          if (a.isInt()) {
            out = EValue(-a.toInt());
          } else if (a.isDouble()) {
            out = EValue(-a.toDouble());
          } else {
            ET_KERNEL_CHECK_MSG(
                context, false, InvalidType, /* void */, "%zu", (size_t)a.tag);
          }
        }),

    // executorch_prim::floordiv.int(int, int) -> int
    Kernel(
        "executorch_prim::floordiv.int",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& a = *stack[0];
          EValue& b = *stack[1];
          EValue& out = *stack[2];
          out = EValue(a.toInt() / b.toInt());
        }),

    // executorch_prim::mod.int(int, int) -> int
    Kernel(
        "executorch_prim::mod.int",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& a = *stack[0];
          EValue& b = *stack[1];
          EValue& out = *stack[2];
          out = EValue(a.toInt() % b.toInt());
        }),

    // executorch_prim::mod.Scalar(Scalar, Scalar) -> Scalar
    Kernel(
        "executorch_prim::mod.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& a = *stack[0];
          EValue& b = *stack[1];
          EValue& out = *stack[2];
          if (a.isInt() && b.isInt()) {
            out = EValue(a.toInt() % b.toInt());
          } else {
            ET_KERNEL_CHECK_MSG(
                context,
                false,
                InvalidType,
                /* void */,
                "%zu, %zu",
                (size_t)a.tag,
                (size_t)b.tag);
          }
        }),

    // ceil.Scalar(Scalar a) -> Scalar
    Kernel(
        "executorch_prim::ceil.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& a = *stack[0];
          EValue& out = *stack[1];
          if (a.isDouble()) {
            out = EValue(static_cast<int64_t>(ceil(a.toDouble())));
          } else {
            ET_KERNEL_CHECK_MSG(
                context,
                false,
                InvalidType,
                /* void */,
                "Unsupported DType %zu",
                (size_t)a.tag);
          }
        }),

    // round.Scalar(Scalar a) -> Scalar
    Kernel(
        "executorch_prim::round.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& a = *stack[0];
          EValue& out = *stack[1];
          if (a.isDouble()) {
            // Round half to even to match Python round(). Need an explicit
            // implementation as not all platforms support fenv rounding modes.
            // See
            // https://codeyarns.com/tech/2018-08-17-how-to-round-half-to-even.html
            const auto val = a.toDouble();
            const auto r = round(val);
            const auto d = r - val;
            auto res = 0.0;

            if (std::abs(d) != 0.5) {
              res = r;
            } else if (fmod(r, 2.0) == 0.0) {
              res = r;
            } else {
              res = val - d;
            }

            out = EValue(static_cast<int64_t>(res));
          } else {
            ET_KERNEL_CHECK_MSG(
                context,
                false,
                InvalidType,
                /* void */,
                "Unsupported DType %zu",
                (size_t)a.tag);
          }
        }),

    // trunc.Scalar(Scalar a) -> Scalar
    Kernel(
        "executorch_prim::trunc.Scalar",
        [](KernelRuntimeContext& context, EValue** stack) {
          (void)context;
          EValue& a = *stack[0];
          EValue& out = *stack[1];
          if (a.isDouble()) {
            out = EValue(static_cast<int64_t>(trunc(a.toDouble())));
          } else {
            ET_KERNEL_CHECK_MSG(
                context, false, InvalidType, /* void */, "%zu", (size_t)a.tag);
          }
        }),

    // executorch_prim::et_copy_index.tensor(tensor, tensor) -> tensor
    Kernel(
        "executorch_prim::et_copy_index.tensor",
        [](KernelRuntimeContext& context, EValue** stack) {
          et_copy_index(context, stack);
        }),
    // executorch_prim::et_view.default(Tensor, int[]) -> Tensor
    Kernel(
        "executorch_prim::et_view.default",
        [](KernelRuntimeContext& context, EValue** stack) {
          et_view(context, stack);
        }),

};

executorch::runtime::Span<const executorch::ET_RUNTIME_NAMESPACE::Kernel>
    kernel_span(prim_ops, prim_ops + sizeof(prim_ops) / sizeof(Kernel));

// Return value not used. Keep the static variable assignment to register
// operators in static initialization time.
auto success_with_kernel_reg =
    executorch::ET_RUNTIME_NAMESPACE::register_kernels(kernel_span);

} // namespace
} // namespace function
} // namespace executor
} // namespace torch
