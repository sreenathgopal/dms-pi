#include <dms/classifier.h>
#include <iostream>
#include <cmath>
#include <algorithm>

#ifndef DMS_NO_TFLITE
#include <tensorflow/lite/interpreter.h>
#include <tensorflow/lite/kernels/register.h>
#include <tensorflow/lite/model.h>
#endif

namespace dms {

struct DrowsinessClassifier::Impl {
#ifndef DMS_NO_TFLITE
    std::unique_ptr<tflite::FlatBufferModel> model;
    std::unique_ptr<tflite::Interpreter> interpreter;
    bool loaded = false;

    // Quantization parameters
    int input_idx = 0;
    int output_idx = 0;
    bool input_quantized = false;
    bool output_quantized = false;
    float input_scale = 1.0f;
    int input_zero_point = 0;
    float output_scale = 1.0f;
    int output_zero_point = 0;
#endif
};

DrowsinessClassifier::DrowsinessClassifier(const std::string& model_path)
    : impl_(std::make_unique<Impl>())
{
#ifndef DMS_NO_TFLITE
    impl_->model = tflite::FlatBufferModel::BuildFromFile(model_path.c_str());
    if (!impl_->model) {
        std::cerr << "[classifier] Failed to load model: " << model_path << std::endl;
        return;
    }

    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder builder(*impl_->model, resolver);
    builder(&impl_->interpreter);

    if (!impl_->interpreter) {
        std::cerr << "[classifier] Failed to create interpreter" << std::endl;
        return;
    }

    impl_->interpreter->SetNumThreads(1);
    if (impl_->interpreter->AllocateTensors() != kTfLiteOk) {
        std::cerr << "[classifier] Failed to allocate tensors" << std::endl;
        return;
    }

    // Get input details
    const auto& input_details = impl_->interpreter->inputs();
    const auto& output_details = impl_->interpreter->outputs();
    impl_->input_idx = input_details[0];
    impl_->output_idx = output_details[0];

    const TfLiteTensor* inp_tensor = impl_->interpreter->tensor(impl_->input_idx);
    const TfLiteTensor* out_tensor = impl_->interpreter->tensor(impl_->output_idx);

    // Check for INT8 quantization
    if (inp_tensor->type == kTfLiteInt8) {
        impl_->input_quantized = true;
        impl_->input_scale = inp_tensor->params.scale;
        impl_->input_zero_point = inp_tensor->params.zero_point;
    }
    if (out_tensor->type == kTfLiteInt8) {
        impl_->output_quantized = true;
        impl_->output_scale = out_tensor->params.scale;
        impl_->output_zero_point = out_tensor->params.zero_point;
    }

    impl_->loaded = true;
    std::cout << "[classifier] Loaded: " << model_path
              << " (input_quant=" << impl_->input_quantized
              << ", scale=" << impl_->input_scale
              << ", zp=" << impl_->input_zero_point << ")" << std::endl;
#else
    (void)model_path;
    std::cerr << "[classifier] Built without TFLite — classifier disabled" << std::endl;
#endif
}

DrowsinessClassifier::~DrowsinessClassifier() = default;

bool DrowsinessClassifier::is_loaded() const {
#ifndef DMS_NO_TFLITE
    return impl_->loaded;
#else
    return false;
#endif
}

std::pair<float, float> DrowsinessClassifier::predict(float ear_left, float ear_right, float mar) {
#ifndef DMS_NO_TFLITE
    if (!impl_->loaded) return {0.0f, 0.0f};

    float input_vals[3] = {ear_left, ear_right, mar};

    if (impl_->input_quantized) {
        // Quantize float -> int8
        int8_t* input_data = impl_->interpreter->typed_tensor<int8_t>(impl_->input_idx);
        for (int i = 0; i < 3; i++) {
            int q = static_cast<int>(std::round(input_vals[i] / impl_->input_scale))
                    + impl_->input_zero_point;
            input_data[i] = static_cast<int8_t>(std::clamp(q, -128, 127));
        }
    } else {
        float* input_data = impl_->interpreter->typed_tensor<float>(impl_->input_idx);
        for (int i = 0; i < 3; i++) {
            input_data[i] = input_vals[i];
        }
    }

    if (impl_->interpreter->Invoke() != kTfLiteOk) {
        return {0.0f, 0.0f};
    }

    float eye_prob, yawn_prob;

    if (impl_->output_quantized) {
        // Dequantize int8 -> float
        const int8_t* output_data = impl_->interpreter->typed_tensor<int8_t>(impl_->output_idx);
        eye_prob = (static_cast<float>(output_data[0]) - impl_->output_zero_point) * impl_->output_scale;
        yawn_prob = (static_cast<float>(output_data[1]) - impl_->output_zero_point) * impl_->output_scale;
    } else {
        const float* output_data = impl_->interpreter->typed_tensor<float>(impl_->output_idx);
        eye_prob = output_data[0];
        yawn_prob = output_data[1];
    }

    return {
        std::clamp(eye_prob, 0.0f, 1.0f),
        std::clamp(yawn_prob, 0.0f, 1.0f)
    };
#else
    (void)ear_left; (void)ear_right; (void)mar;
    return {0.0f, 0.0f};
#endif
}

} // namespace dms
