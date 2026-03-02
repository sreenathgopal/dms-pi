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

// Face detector: tries YuNet first, falls back to Haar cascade if YuNet
// fails (e.g. OpenCV 4.6 ONNX incompatibility).
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

    bool using_haar() const { return use_haar_; }

private:
    cv::Ptr<cv::FaceDetectorYN> detector_;
    cv::CascadeClassifier haar_;
    bool use_haar_ = false;
    float threshold_;
};

} // namespace dms
