#pragma once

#include <dms/face_detector.h>
#include <opencv2/core.hpp>
#include <string>
#include <vector>
#include <array>
#include <memory>

namespace dms {

struct LandmarkResult {
    bool face_detected = false;
    float ear_left = 0;
    float ear_right = 0;
    float mar = 0;
    std::string gaze = "center";

    // Raw 478 landmarks (x,y,z normalized to crop)
    std::vector<std::array<float, 3>> landmarks;
};

// TFLite-based face landmark tracker (478 landmarks, same as MediaPipe FaceMesh)
class LandmarkTracker {
public:
    explicit LandmarkTracker(const std::string& model_path, float bbox_expand = 0.2f);
    ~LandmarkTracker();

    // Process a face crop: detect 478 landmarks and compute EAR/MAR/gaze
    LandmarkResult process(const cv::Mat& frame, const FaceBox& face);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    float bbox_expand_;

    // Compute EAR from 6 landmark indices
    static float compute_ear(const std::vector<std::array<float, 3>>& lm,
                              const std::array<int, 6>& indices,
                              float w, float h);

    // Compute MAR from 4 landmark indices
    static float compute_mar(const std::vector<std::array<float, 3>>& lm,
                              const std::array<int, 4>& indices,
                              float w, float h);

    // Compute gaze direction from iris landmarks
    static std::string compute_gaze(const std::vector<std::array<float, 3>>& lm,
                                     float w, float h);
};

} // namespace dms
