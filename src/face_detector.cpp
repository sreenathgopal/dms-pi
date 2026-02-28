#include <dms/face_detector.h>
#include <iostream>

namespace dms {

FaceDetector::FaceDetector(const std::string& model_path,
                           int input_w, int input_h,
                           float confidence_threshold)
    : threshold_(confidence_threshold)
{
    detector_ = cv::FaceDetectorYN::create(
        model_path,
        "",                      // config (empty for ONNX)
        cv::Size(input_w, input_h),
        confidence_threshold,
        0.3f,                    // NMS threshold
        5000                     // top_k
    );

    if (!detector_) {
        std::cerr << "[face_detector] Failed to load YuNet model: " << model_path << std::endl;
    }
}

void FaceDetector::set_input_size(int w, int h) {
    if (detector_) {
        detector_->setInputSize(cv::Size(w, h));
    }
}

bool FaceDetector::detect(const cv::Mat& frame, FaceBox& best_face) {
    if (!detector_) return false;

    cv::Mat faces;
    detector_->detect(frame, faces);

    if (faces.empty() || faces.rows == 0) return false;

    // Find highest confidence face
    int best_idx = 0;
    float best_conf = -1.0f;
    for (int i = 0; i < faces.rows; i++) {
        float conf = faces.at<float>(i, 14);  // confidence at column 14
        if (conf > best_conf) {
            best_conf = conf;
            best_idx = i;
        }
    }

    if (best_conf < threshold_) return false;

    best_face.x = static_cast<int>(faces.at<float>(best_idx, 0));
    best_face.y = static_cast<int>(faces.at<float>(best_idx, 1));
    best_face.w = static_cast<int>(faces.at<float>(best_idx, 2));
    best_face.h = static_cast<int>(faces.at<float>(best_idx, 3));
    best_face.confidence = best_conf;

    return true;
}

} // namespace dms
