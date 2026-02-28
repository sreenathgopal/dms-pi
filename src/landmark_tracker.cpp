#include <dms/landmark_tracker.h>
#include <dms/config.h>
#include <opencv2/imgproc.hpp>
#include <cmath>
#include <iostream>
#include <algorithm>

#ifndef DMS_NO_TFLITE
#include <tensorflow/lite/interpreter.h>
#include <tensorflow/lite/kernels/register.h>
#include <tensorflow/lite/model.h>
#endif

namespace dms {

static constexpr int LANDMARK_INPUT_SIZE = 192;
static constexpr int NUM_LANDMARKS = 478;

struct LandmarkTracker::Impl {
#ifndef DMS_NO_TFLITE
    std::unique_ptr<tflite::FlatBufferModel> model;
    std::unique_ptr<tflite::Interpreter> interpreter;
    bool loaded = false;
#endif
};

LandmarkTracker::LandmarkTracker(const std::string& model_path, float bbox_expand)
    : impl_(std::make_unique<Impl>()), bbox_expand_(bbox_expand)
{
#ifndef DMS_NO_TFLITE
    impl_->model = tflite::FlatBufferModel::BuildFromFile(model_path.c_str());
    if (!impl_->model) {
        std::cerr << "[landmark] Failed to load model: " << model_path << std::endl;
        return;
    }

    tflite::ops::builtin::BuiltinOpResolver resolver;
    tflite::InterpreterBuilder builder(*impl_->model, resolver);
    builder(&impl_->interpreter);

    if (!impl_->interpreter) {
        std::cerr << "[landmark] Failed to create interpreter" << std::endl;
        return;
    }

    impl_->interpreter->SetNumThreads(1);
    if (impl_->interpreter->AllocateTensors() != kTfLiteOk) {
        std::cerr << "[landmark] Failed to allocate tensors" << std::endl;
        return;
    }

    impl_->loaded = true;
    std::cout << "[landmark] Model loaded: " << model_path << std::endl;
#else
    (void)model_path;
    std::cerr << "[landmark] Built without TFLite — landmark tracking disabled" << std::endl;
#endif
}

LandmarkTracker::~LandmarkTracker() = default;

float LandmarkTracker::compute_ear(const std::vector<std::array<float, 3>>& lm,
                                    const std::array<int, 6>& idx,
                                    float w, float h)
{
    // EAR = (|P2-P6| + |P3-P5|) / (2 * |P1-P4|)
    auto dist = [&](int a, int b) -> float {
        float dx = lm[a][0] * w - lm[b][0] * w;
        float dy = lm[a][1] * h - lm[b][1] * h;
        return std::sqrt(dx * dx + dy * dy);
    };

    float v1 = dist(idx[1], idx[5]);
    float v2 = dist(idx[2], idx[4]);
    float h_dist = dist(idx[0], idx[3]);

    if (h_dist < 1e-6f) return 0.0f;
    return (v1 + v2) / (2.0f * h_dist);
}

float LandmarkTracker::compute_mar(const std::vector<std::array<float, 3>>& lm,
                                    const std::array<int, 4>& idx,
                                    float w, float h)
{
    auto dist = [&](int a, int b) -> float {
        float dx = lm[a][0] * w - lm[b][0] * w;
        float dy = lm[a][1] * h - lm[b][1] * h;
        return std::sqrt(dx * dx + dy * dy);
    };

    float vertical = dist(idx[0], idx[1]);
    float horizontal = dist(idx[2], idx[3]);

    if (horizontal < 1e-6f) return 0.0f;
    return vertical / horizontal;
}

std::string LandmarkTracker::compute_gaze(const std::vector<std::array<float, 3>>& lm,
                                           float w, float h)
{
    const auto& cfg = config();
    if (lm.size() < 478) return "center";

    float l_iris_x = lm[cfg.LEFT_IRIS[0]][0] * w;
    float l_outer  = lm[cfg.LEFT_EYE[0]][0] * w;
    float l_inner  = lm[cfg.LEFT_EYE[3]][0] * w;
    float l_eye_w  = std::abs(l_inner - l_outer);

    if (l_eye_w < 1e-6f) return "center";
    float ratio = (l_iris_x - l_outer) / l_eye_w;

    if (ratio < cfg.gaze_left_ratio) return "right";
    if (ratio > cfg.gaze_right_ratio) return "left";
    return "center";
}

LandmarkResult LandmarkTracker::process(const cv::Mat& frame, const FaceBox& face) {
    LandmarkResult result;

#ifndef DMS_NO_TFLITE
    if (!impl_->loaded) return result;

    int fh = frame.rows;
    int fw = frame.cols;

    // Expand bbox by bbox_expand_ factor
    int expand_w = static_cast<int>(face.w * bbox_expand_);
    int expand_h = static_cast<int>(face.h * bbox_expand_);
    int x1 = std::max(0, face.x - expand_w);
    int y1 = std::max(0, face.y - expand_h);
    int x2 = std::min(fw, face.x + face.w + expand_w);
    int y2 = std::min(fh, face.y + face.h + expand_h);

    cv::Mat crop = frame(cv::Rect(x1, y1, x2 - x1, y2 - y1));

    // Resize to 192x192, BGR->RGB, normalize to [0, 1]
    cv::Mat resized;
    cv::resize(crop, resized, cv::Size(LANDMARK_INPUT_SIZE, LANDMARK_INPUT_SIZE));
    cv::cvtColor(resized, resized, cv::COLOR_BGR2RGB);

    // Get input tensor and fill
    float* input_data = impl_->interpreter->typed_input_tensor<float>(0);
    if (!input_data) return result;

    // Convert uint8 [0,255] -> float32 [0,1]
    for (int y = 0; y < LANDMARK_INPUT_SIZE; y++) {
        const uint8_t* row = resized.ptr<uint8_t>(y);
        for (int x = 0; x < LANDMARK_INPUT_SIZE; x++) {
            int base_src = x * 3;
            int base_dst = (y * LANDMARK_INPUT_SIZE + x) * 3;
            input_data[base_dst + 0] = row[base_src + 0] / 255.0f;
            input_data[base_dst + 1] = row[base_src + 1] / 255.0f;
            input_data[base_dst + 2] = row[base_src + 2] / 255.0f;
        }
    }

    // Run inference
    if (impl_->interpreter->Invoke() != kTfLiteOk) {
        std::cerr << "[landmark] Inference failed" << std::endl;
        return result;
    }

    // Parse output: 478 landmarks * 3 (x, y, z) normalized to input size
    // Output tensor 0 is typically the landmarks (1, 1404) -> 468*3
    // or (1, 1434) -> 478*3 with attention
    const TfLiteTensor* output_tensor = impl_->interpreter->output_tensor(0);
    if (!output_tensor) return result;

    int total_floats = 1;
    for (int i = 0; i < output_tensor->dims->size; i++) {
        total_floats *= output_tensor->dims->data[i];
    }
    int num_landmarks = total_floats / 3;
    if (num_landmarks < 468) return result;
    // Cap at 478
    num_landmarks = std::min(num_landmarks, NUM_LANDMARKS);

    const float* output_data = impl_->interpreter->typed_output_tensor<float>(0);
    if (!output_data) return result;

    // Extract landmarks normalized to [0, 1]
    result.landmarks.resize(num_landmarks);
    float crop_w = static_cast<float>(x2 - x1);
    float crop_h = static_cast<float>(y2 - y1);

    for (int i = 0; i < num_landmarks; i++) {
        // Landmark coords are in input image space (192x192)
        float lx = output_data[i * 3 + 0] / LANDMARK_INPUT_SIZE;
        float ly = output_data[i * 3 + 1] / LANDMARK_INPUT_SIZE;
        float lz = output_data[i * 3 + 2] / LANDMARK_INPUT_SIZE;
        result.landmarks[i] = {lx, ly, lz};
    }

    result.face_detected = true;

    // Compute EAR/MAR/gaze using the crop dimensions
    const auto& cfg = config();
    result.ear_left = compute_ear(result.landmarks, cfg.LEFT_EYE, crop_w, crop_h);
    result.ear_right = compute_ear(result.landmarks, cfg.RIGHT_EYE, crop_w, crop_h);
    result.mar = compute_mar(result.landmarks, cfg.MOUTH, crop_w, crop_h);
    if (num_landmarks >= 478) {
        result.gaze = compute_gaze(result.landmarks, crop_w, crop_h);
    }
#else
    (void)frame;
    (void)face;
#endif

    return result;
}

} // namespace dms
