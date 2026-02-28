#pragma once

#include <opencv2/objdetect.hpp>
#include <opencv2/core.hpp>
#include <string>

namespace dms {

// Bounding box for a detected face
struct FaceBox {
    int x, y, w, h;
    float confidence;
};

// YuNet-based face detector using OpenCV's cv::FaceDetectorYN
class FaceDetector {
public:
    explicit FaceDetector(const std::string& model_path,
                          int input_w = 320, int input_h = 240,
                          float confidence_threshold = 0.5f);

    // Detect faces in a BGR frame. Returns true if at least one face found.
    // On success, best_face is set to the highest-confidence detection.
    bool detect(const cv::Mat& frame, FaceBox& best_face);

    // Update input size (if frame resolution changes)
    void set_input_size(int w, int h);

private:
    cv::Ptr<cv::FaceDetectorYN> detector_;
    float threshold_;
};

} // namespace dms
