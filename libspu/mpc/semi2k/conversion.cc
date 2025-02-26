// Copyright 2021 Ant Group Co., Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "libspu/mpc/semi2k/conversion.h"

#include "libspu/core/vectorize.h"
#include "libspu/mpc/ab_api.h"
#include "libspu/mpc/common/communicator.h"
#include "libspu/mpc/common/prg_state.h"
#include "libspu/mpc/semi2k/state.h"
#include "libspu/mpc/semi2k/type.h"
#include "libspu/mpc/utils/ring_ops.h"

namespace spu::mpc::semi2k {

static NdArrayRef wrap_add_bb(SPUContext* ctx, const NdArrayRef& x,
                              const NdArrayRef& y) {
  SPU_ENFORCE(x.shape() == y.shape());
  return UnwrapValue(add_bb(ctx, WrapValue(x), WrapValue(y)));
}

static NdArrayRef wrap_a2b(SPUContext* ctx, const NdArrayRef& x) {
  return UnwrapValue(a2b(ctx, WrapValue(x)));
}

NdArrayRef A2B::proc(KernelEvalContext* ctx, const NdArrayRef& x) const {
  const auto field = x.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();
  auto* prg_state = ctx->getState<PrgState>();

  std::vector<NdArrayRef> bshrs;
  const auto bty = makeType<BShrTy>(field);
  for (size_t idx = 0; idx < comm->getWorldSize(); idx++) {
    auto [r0, r1] = prg_state->genPrssPair(field, x.shape());
    auto b = ring_xor(r0, r1).as(bty);

    if (idx == comm->getRank()) {
      ring_xor_(b, x);
    }
    bshrs.push_back(b.as(bty));
  }

  NdArrayRef res = vreduce(bshrs.begin(), bshrs.end(),
                           [&](const NdArrayRef& xx, const NdArrayRef& yy) {
                             return wrap_add_bb(ctx->sctx(), xx, yy);
                           });
  return res.as(bty);
}

NdArrayRef B2A::proc(KernelEvalContext* ctx, const NdArrayRef& x) const {
  const auto field = x.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();
  auto* prg_state = ctx->getState<PrgState>();

  auto r_v = prg_state->genPriv(field, x.shape());
  auto r_a = r_v.as(makeType<AShrTy>(field));

  // convert r to boolean share.
  auto r_b = wrap_a2b(ctx->sctx(), r_a);

  // evaluate adder circuit on x & r, and reveal x+r
  auto x_plus_r = comm->allReduce(ReduceOp::XOR,
                                  wrap_add_bb(ctx->sctx(), x, r_b), kBindName);

  // compute -r + (x+r)
  ring_neg_(r_a);
  if (comm->getRank() == 0) {
    ring_add_(r_a, x_plus_r);
  }
  return r_a;
}

NdArrayRef B2A_Randbit::proc(KernelEvalContext* ctx,
                             const NdArrayRef& x) const {
  const auto field = x.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();
  auto* beaver = ctx->getState<Semi2kState>()->beaver();

  const int64_t nbits = x.eltype().as<BShare>()->nbits();
  SPU_ENFORCE((size_t)nbits <= SizeOf(field) * 8, "invalid nbits={}", nbits);
  if (nbits == 0) {
    // special case, it's known to be zero.
    return ring_zeros(field, x.shape()).as(makeType<AShrTy>(field));
  }

  auto numel = x.numel();

  auto randbits = beaver->RandBit(field, {numel * static_cast<int64_t>(nbits)});
  auto res = NdArrayRef(makeType<AShrTy>(field), x.shape());

  DISPATCH_ALL_FIELDS(field, kBindName, [&]() {
    using U = ring2k_t;

    // algorithm begins.
    // Ref: III.D @ https://eprint.iacr.org/2019/599.pdf (SPDZ-2K primitives)
    std::vector<U> x_xor_r(numel);

    pforeach(0, numel, [&](int64_t idx) {
      // use _r[i*nbits, (i+1)*nbits) to construct rb[i]
      U mask = 0;
      for (int64_t bit = 0; bit < nbits; ++bit) {
        mask += (randbits.at<U>(idx * nbits + bit) & 0x1) << bit;
      }
      x_xor_r[idx] = x.at<U>(idx) ^ mask;
    });

    // open c = x ^ r
    x_xor_r = comm->allReduce<U, std::bit_xor>(x_xor_r, "open(x^r)");

    pforeach(0, numel, [&](int64_t idx) {
      res.at<U>(idx) = 0;
      for (int64_t bit = 0; bit < nbits; bit++) {
        auto c_i = (x_xor_r[idx] >> bit) & 0x1;
        if (comm->getRank() == 0) {
          res.at<U>(idx) +=
              (c_i + (1 - c_i * 2) * randbits.at<U>(idx * nbits + bit)) << bit;
        } else {
          res.at<U>(idx) += ((1 - c_i * 2) * randbits.at<U>(idx * nbits + bit))
                            << bit;
        }
      }
    });
  });

  return res;
}

NdArrayRef MsbA2B::proc(KernelEvalContext* ctx, const NdArrayRef& in) const {
  const auto field = in.eltype().as<Ring2k>()->field();
  auto* comm = ctx->getState<Communicator>();
  auto* prg_state = ctx->getState<PrgState>();

  // For if k > 2 parties does not collude with each other, then we can
  // construct two additive share and use carray out circuit directly.
  SPU_ENFORCE(comm->getWorldSize() == 2, "only support for 2PC, got={}",
              comm->getWorldSize());

  std::vector<NdArrayRef> bshrs;
  const auto bty = makeType<BShrTy>(field);
  for (size_t idx = 0; idx < comm->getWorldSize(); idx++) {
    auto [r0, r1] = prg_state->genPrssPair(field, in.shape());
    auto b = ring_xor(r0, r1).as(bty);
    if (idx == comm->getRank()) {
      ring_xor_(b, in);
    }
    bshrs.push_back(b.as(bty));
  }

  // Compute the k-1'th carry bit.
  size_t k = SizeOf(field) * 8 - 1;
  if (in.numel() == 0) {
    k = 0;  // Empty matrix
  }

  auto* sctx = ctx->sctx();
  const Shape shape = {in.numel()};
  auto m = WrapValue(bshrs[0]);
  auto n = WrapValue(bshrs[1]);
  {
    auto carry = carry_a2b(sctx, m, n, k);

    // Compute the k'th bit.
    //   (m^n)[k] ^ carry
    auto msb = xor_bb(sctx, rshift_b(sctx, xor_bb(sctx, m, n), k), carry);

    return UnwrapValue(msb);
  }
}

}  // namespace spu::mpc::semi2k
