#include "torch_ipex/csrc/utils.h"

#include <ATen/NativeFunctions.h>
#include <torch/csrc/autograd/function.h>

#include "Config.hpp"
// #include "Observer.hpp"

namespace torch_ipex {
using namespace int8;

void Int8OptConfig::insert_or_updata_observer(
    std::string op_name, std::vector<std::vector<float>> i_min_max_values,
    std::vector<std::vector<float>> w_min_max_values,
    std::vector<std::vector<float>> o_min_max_values, int64_t ops_id,
    std::vector<std::string> inputs_flow, std::vector<std::string> outputs_flow) {
  if (observers_.size() <= ops_id) {
    // this path is that to set int8 op's configure, using default configures if
    // user not set it. Note: weight's value only set onece.
    std::string observer_algorithm = "min_max";
    float averaging_constant = 0.01; // will be enabled for moving_averager_min_max
    std::string weight_granularity = "per_channel";
    const int nums_input = i_min_max_values.size();
    const int nums_output = o_min_max_values.size();
    std::vector<std::string> input_quantized_dtypes(nums_input, "uint8");
    std::vector<std::string> output_quantized_dtypes(nums_output, "uint8");
    const auto num_inputs = i_min_max_values.size();
    std::vector<bool> inputs_quantized(num_inputs, true);
    const auto num_outputs = o_min_max_values.size();
    std::vector<bool> outputs_quantized(num_outputs, true);
    if (op_name == "relu_" || op_name == "add_") {
      std::fill(inputs_quantized.begin(), inputs_quantized.end(), false);
      std::fill(outputs_quantized.begin(), outputs_quantized.end(), false);
    }
    if (!indicators_.empty()) {
      observer_algorithm = indicators_[ops_id].get_indicator_algorithm();
      weight_granularity =
          indicators_[ops_id].get_indicator_weight_granularity();
      std::tie(input_quantized_dtypes, output_quantized_dtypes) =
          indicators_[ops_id].get_indicator_quantized_dtypes();
      std::tie(inputs_quantized, outputs_quantized) =
          indicators_[ops_id].get_indicator_insert_quantized_status();
    }
    Observer new_observer = {ops_id,
                             op_name,
                             i_min_max_values,
                             w_min_max_values,
                             o_min_max_values,
                             observer_algorithm,
                             averaging_constant,
                             weight_granularity,
                             input_quantized_dtypes,
                             output_quantized_dtypes,
                             inputs_quantized,
                             outputs_quantized,
                             inputs_flow,
                             outputs_flow};
    observers_.push_back(new_observer);
  } else {
    // user has set configure or have run one interation
    auto inputs_pre = observers_[ops_id].inputs_min_max_values;
    auto outputs_pre = observers_[ops_id].outputs_min_max_values;
    if (observers_[ops_id].algorithm == "min_max") {
      for (auto i = 0; i < i_min_max_values.size(); i++) {
        observers_[ops_id].inputs_min_max_values[i][0] =
            std::min(inputs_pre[i][0], i_min_max_values[i][0]);
        observers_[ops_id].inputs_min_max_values[i][1] =
            std::max(inputs_pre[i][1], i_min_max_values[i][1]);
      }
      for (auto j = 0; j < o_min_max_values.size(); j++) {
        observers_[ops_id].outputs_min_max_values[j][0] =
            std::min(outputs_pre[j][0], o_min_max_values[j][0]);
        observers_[ops_id].outputs_min_max_values[j][1] =
            std::max(outputs_pre[j][1], o_min_max_values[j][1]);
      }
    } else if (observers_[ops_id].algorithm == "moving_averager_min_max") {
      auto c = observers_[ops_id].averaging_constant;
      for (auto i = 0; i < i_min_max_values.size(); i++) {
        observers_[ops_id].inputs_min_max_values[i][0] =
            (1 - c) * inputs_pre[i][0] + c * i_min_max_values[i][0];
        observers_[ops_id].inputs_min_max_values[i][1] =
            (1 - c) * inputs_pre[i][1] + c * i_min_max_values[i][1];
      }
      for (auto j = 0; j < o_min_max_values.size(); j++) {
        observers_[ops_id].outputs_min_max_values[j][0] =
            (1 - c) * outputs_pre[j][0] + c * o_min_max_values[j][0];
        observers_[ops_id].outputs_min_max_values[j][1] =
            (1 - c) * outputs_pre[j][1] + c * o_min_max_values[j][1];
      }
    }
  }
}

void Int8OptConfig::clear_indicators() { indicators_.clear(); }

void Int8OptConfig::add_indicators() {
  indicators_.clear();
  // default used is u8
  const int precision = 8;
  for (auto i = 0; i < observers_.size(); i++) {
    std::vector<quant_utils::TensorQuantizationParams> input_params, output_params;
    std::vector<float> weight_scales;

    std::vector<std::vector<float>> input_values = observers_[i].inputs_min_max_values;
    std::vector<std::vector<float>> output_values = observers_[i].outputs_min_max_values;
    std::vector<std::vector<float>> weight_values = observers_[i].weight_min_max_values;
    std::vector<std::string> x_quantized_types = observers_[i].input_quantized_dtypes;
    std::vector<std::string> y_quantized_types = observers_[i].output_quantized_dtypes;
    // for symmetric: s = 2max(|x_min|, x_max) / (Q_max - Q_min),
    // z = 0 for qint8 and z = 128 for quint8;
    // otherwise: s = (x_max - x_min) / (Q_max - Q_min),
    // z = Q_min - round(x_min / s).
    for (auto j = 0; j < input_values.size(); j++) {
      bool is_signed = x_quantized_types[j] == "int8" ? true : false;
      auto qparams = quant_utils::ChooseQuantizationParams(
          /*min*/ input_values[j][0],
          /*max*/ input_values[j][1],
          /*q_min*/ is_signed ? -(1 << (precision - 1)) : 0,
          /*q_max*/ is_signed ? ((1 << (precision - 1)) - 1) : (1 << precision) - 1
      );
      input_params.push_back(qparams);
    }
    for (auto k = 0; k < output_values.size(); k++) {
      bool is_signed = y_quantized_types[k] == "int8" ? true : false;
      auto qparams = quant_utils::ChooseQuantizationParams(
          /*min*/ output_values[k][0],
          /*max*/ output_values[k][1],
          /*q_min*/ is_signed ? -(1 << (precision - 1)) : 0,
          /*q_max*/ is_signed ? ((1 << (precision - 1)) - 1) : (1 << precision) - 1);
      output_params.push_back(qparams);
    }
    // for weight, always using symetric quantization, quantized to int8 dtype.
    // is_signed = true;
    for (auto m = 0; m < weight_values.size(); m++) {
      auto max_value = std::max(std::abs(weight_values[m][0]), weight_values[m][1]);
      auto qparams = quant_utils::ChooseQuantizationParams(
          /*min*/ -max_value,
          /*max*/ max_value,
          /*q_min*/ -(1 << (precision - 1)),
          /*q_max*/ ((1 << (precision - 1)) - 1));
      weight_scales.push_back(qparams.scale);
    }
    Indicator new_indicator(
        observers_[i].id, observers_[i].name, observers_[i].algorithm,
        observers_[i].weight_granularity, input_params, weight_scales, output_params,
        observers_[i].input_quantized_dtypes, observers_[i].output_quantized_dtypes,
        observers_[i].inputs_quantized, observers_[i].outputs_quantized,
        observers_[i].inputs_flow, observers_[i].outputs_flow);
    indicators_.push_back(new_indicator);
  }
  observers_.clear();
}

std::vector<std::vector<quant_utils::TensorQuantizationParams>> Int8OptConfig::get_indicator_scales(const int64_t ops_id) {
  std::vector<quant_utils::TensorQuantizationParams> x_params, y_params;
  std::tie(x_params, y_params) = indicators_[ops_id].get_indicator_scales();
  return  {x_params, y_params};
}

std::string Int8OptConfig::get_indicator_weight_granularity(const int64_t ops_id) {
  std::string weight_granularity = "per_channel";
  // user not set weight granularity, using default granularity
  if (indicators_.empty()) {
    return weight_granularity;
  }

  weight_granularity = indicators_[ops_id].get_indicator_weight_granularity();
  return weight_granularity;
}

float Int8OptConfig::get_indicator_weight_scale(const int64_t ops_id) {
  return indicators_[ops_id].get_indicator_weight_scales()[0];
}

at::Tensor& Int8OptConfig::get_indicator_weight_tensor_scale(const int64_t ops_id) {
  return weights_scales_[ops_id];
}

std::tuple<std::vector<bool>, std::vector<bool>>
Int8OptConfig::get_indicator_insert_quantized_status(const int64_t ops_id) {
   return indicators_[ops_id].get_indicator_insert_quantized_status();
}

std::tuple<std::vector<std::string>, std::vector<std::string>>
Int8OptConfig::get_indicator_quantized_dtypes(const int64_t ops_id) {
   return indicators_[ops_id].get_indicator_quantized_dtypes();
}

void Int8OptConfig::set_indicators(std::vector<Indicator> indicators) {
  // avoid to use copy assignment since the copy assignment for indicator with rw_mutex
  // have not been handdled properly
  indicators_.reserve(indicators.size());
  for (auto i: indicators){
    // if weight_granularity is per_channle, first cache the scales tensor for trace.
    if (i.get_indicator_weight_granularity() == "per_channel") {
      auto id = i.get_indicator_id();
      auto w_scales = i.get_indicator_weight_scales();
      auto casted_scale = at::tensor(w_scales, at::device(at::kCPU).dtype(at::kDouble));
      weights_scales_.emplace(id, casted_scale);
    }
    indicators_.emplace_back(i);
  }
}

std::vector<Indicator> Int8OptConfig::get_indicators() { return indicators_; }

int64_t Int8OptConfig::get_indicators_size() { return indicators_.size(); }

void Int8OptConfig::calibration_reset() { current_ops_id = 0; }

int64_t Int8OptConfig::fetch_and_add_ops_id() {
  int64_t ops_id = current_ops_id++;
  int64_t indicator_size = Int8OptConfig::get_config().get_indicators_size();
  if (current_ops_id == indicator_size)
    current_ops_id = 0;
  return ops_id;
}

thread_local int64_t Int8OptConfig::current_ops_id = 0;

} // namespace torch_ipex