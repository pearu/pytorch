#include <ATen/native/layer_norm.h>

#include <array>
#include <functional>
#include <numeric>
#include <tuple>
#include <vector>

#include <ATen/ATen.h>
#include <ATen/AccumulateType.h>
#include <ATen/CPUApplyUtils.h>
#include <ATen/Config.h>
#include <ATen/NativeFunctions.h>
#include <ATen/Parallel.h>
#include <torch/library.h>

namespace at {
namespace native {

void layer_norm_cpu_out(
    at::Tensor& out,
    at::Tensor& mean,
    at::Tensor& rstd,
    const at::Tensor& input,
    IntArrayRef normalized_shape,
    const Tensor& gamma,
    const Tensor& beta,
    double eps,
    int64_t M,
    int64_t N) {
  if (M <= 0) {
    return;
  }

  LayerNormKernel(kCPU, input, gamma, beta, M, N, eps, &out, &mean, &rstd);
  const auto input_shape = input.sizes();
  const size_t axis = input.dim() - normalized_shape.size();

  DimVector stat_shape;
  for (size_t idx = 0; idx < axis; ++idx) {
    stat_shape.emplace_back(input_shape[idx]);
  }
  for (size_t idx = axis; idx < input.dim(); ++idx) {
    stat_shape.emplace_back(1);
  }

  mean = mean.view(stat_shape);
  rstd = rstd.view(stat_shape);
}

std::tuple<Tensor, Tensor, Tensor> layer_norm_cpu(
    const Tensor& input,
    IntArrayRef normalized_shape, const c10::optional<Tensor>& weight_opt /* optional */, const c10::optional<Tensor>& bias_opt /* optional */,
    double eps) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  c10::MaybeOwned<Tensor> bias_maybe_owned = at::borrow_from_optional_tensor(bias_opt);
  const Tensor& bias = *bias_maybe_owned;


  auto inputs = _prepare_layer_norm_inputs(input, normalized_shape, weight, bias);
  auto X = std::get<0>(inputs);
  auto gamma = std::get<1>(inputs);
  auto beta = std::get<2>(inputs);
  auto M = std::get<3>(inputs);
  auto N = std::get<4>(inputs);

  Tensor Y = at::native::empty_like(
      X,
      c10::nullopt /* dtype */,
      c10::nullopt /* layout */,
      c10::nullopt /* device */,
      c10::nullopt /* pin_memory */,
      at::MemoryFormat::Contiguous);
  Tensor mean = at::empty({M}, X.options());
  Tensor rstd = at::empty({M}, X.options());

  layer_norm_cpu_out(Y, mean, rstd, X, normalized_shape, gamma, beta, eps, M, N);
  return std::make_tuple(std::move(Y), std::move(mean), std::move(rstd));
}

std::tuple<Tensor, Tensor, Tensor> layer_norm_backward_cpu(
    const Tensor& dY,
    const Tensor& input,
    IntArrayRef normalized_shape,
    const Tensor& mean,
    const Tensor& rstd, const c10::optional<Tensor>& weight_opt /* optional */, const c10::optional<Tensor>& bias_opt /* optional */,
    std::array<bool, 3> grad_input_mask) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& bias = c10::value_or_else(bias_opt, [] {return Tensor();});


    auto inputs = _prepare_layer_norm_inputs(input, normalized_shape, weight, bias);
    auto X = std::get<0>(inputs);
    auto gamma = std::get<1>(inputs);
    auto beta = std::get<2>(inputs);
    auto M = std::get<3>(inputs);
    auto N = std::get<4>(inputs);

    Tensor dX;
    Tensor dgamma;
    Tensor dbeta;
    if (grad_input_mask[0]) {
      dX = at::native::empty_like(
          X,
          c10::nullopt /* dtype */,
          c10::nullopt /* layout */,
          c10::nullopt /* device */,
          c10::nullopt /* pin_memory */,
          at::MemoryFormat::Contiguous);
    }
    if (grad_input_mask[1]) {
      dgamma = M > 0 ? at::native::empty_like(
                           gamma,
                           c10::nullopt /* dtype */,
                           c10::nullopt /* layout */,
                           c10::nullopt /* device */,
                           c10::nullopt /* pin_memory */,
                           at::MemoryFormat::Contiguous)
                     : at::native::zeros_like(
                           gamma,
                           c10::nullopt /* dtype */,
                           c10::nullopt /* layout */,
                           c10::nullopt /* device */,
                           c10::nullopt /* pin_memory */,
                           at::MemoryFormat::Contiguous);
    }
    if (grad_input_mask[2]) {
      dbeta = M > 0 ? at::native::empty_like(
                          beta,
                          c10::nullopt /* dtype */,
                          c10::nullopt /* layout */,
                          c10::nullopt /* device */,
                          c10::nullopt /* pin_memory */,
                          at::MemoryFormat::Contiguous)
                    : at::native::zeros_like(
                          beta,
                          c10::nullopt /* dtype */,
                          c10::nullopt /* layout */,
                          c10::nullopt /* device */,
                          c10::nullopt /* pin_memory */,
                          at::MemoryFormat::Contiguous);
    }
    if (M > 0) {
      LayerNormBackwardKernel(
          kCPU, dY, X, mean, rstd, gamma, M, N, &dX, &dgamma, &dbeta);
    }
    return std::make_tuple(std::move(dX), std::move(dgamma), std::move(dbeta));
}

Tensor layer_norm(
    const Tensor& input,
    IntArrayRef normalized_shape, const c10::optional<Tensor>& weight_opt /* optional */, const c10::optional<Tensor>& bias_opt /* optional */,
    double eps,
    bool /* cudnn_enable, deprecated */) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& bias = c10::value_or_else(bias_opt, [] {return Tensor();});


  return std::get<0>(at::native_layer_norm(input, normalized_shape, weight, bias, eps));
}

DEFINE_DISPATCH(LayerNormKernel);
DEFINE_DISPATCH(LayerNormBackwardKernel);

// Ported from pytorch/xla repo
std::tuple<Tensor, Tensor, Tensor> math_native_layer_norm(
    const Tensor& input,
    IntArrayRef normalized_shape, const c10::optional<Tensor>& weight_opt, const c10::optional<Tensor>& bias_opt,
    double eps) {
  // See [Note: hacky wrapper removal for optional tensor]
  c10::MaybeOwned<Tensor> weight_maybe_owned = at::borrow_from_optional_tensor(weight_opt);
  const Tensor& weight = *weight_maybe_owned;
  const Tensor& bias = c10::value_or_else(bias_opt, [] {return Tensor();});

  auto inputs = _prepare_layer_norm_inputs(input, normalized_shape, weight, bias);
  auto X = std::get<0>(inputs);
  auto gamma = std::get<1>(inputs);
  auto beta = std::get<2>(inputs);
  auto M = std::get<3>(inputs);
  auto N = std::get<4>(inputs);
  auto input_shape = input.sizes();
  const auto input_ndim = input.dim();
  const int normalized_ndim = normalized_shape.size();
  const int axis = input_ndim - normalized_ndim;
  at::Tensor input_reshaped = input.view({1, M, -1});
  // Unlike Batch Normalization, which applies scalar scale and bias for each
  // entire channel/plane with the affine option, Layer Normalization applies
  // per-element scale and bias. E.g. For input {N, C, H, W}, weight for
  // batchnorm has shape {C} while weight for layernorm has shape {H, W} or {W}.
  auto outputs = at::native_batch_norm(
      input_reshaped, /*weight=*/{}, /*bias=*/{}, /*running_mean=*/{},
      /*running_var=*/{}, /*training=*/true, /*momentum=*/0, eps);
  at::Tensor out = std::get<0>(outputs);
  out = out.view(input_shape);
  if (weight.defined() && bias.defined()) {
    out = bias.addcmul(out, weight, 1);
  } else if (weight.defined()) {
    out = out.mul(weight);
  } else if (bias.defined()) {
    out = out.add(bias);
  }
  at::Tensor mean = std::get<1>(outputs);
  at::Tensor rstd = std::get<2>(outputs);
  std::vector<int64_t> stat_shape;
  for (size_t idx = 0; idx < axis; ++idx) {
    stat_shape.push_back(input_shape[idx]);
  }
  for (size_t idx = axis; idx < input.dim(); ++idx) {
    stat_shape.push_back(1);
  }
  mean = mean.view(stat_shape);
  rstd = rstd.view(stat_shape);
  return std::make_tuple(out, mean, rstd);
}
} // namespace native
} // namespace at
