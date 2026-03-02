#include <dms/face_detector.h>
#include <opencv2/imgproc.hpp>
#include <iostream>

namespace dms {

FaceDetector::FaceDetector(const std::string& model_path,
                           int input_w, int input_h,
                           float confidence_threshold)
    : threshold_(confidence_threshold)
{
    // Try YuNet first
    try {
        detector_ = cv::FaceDetectorYN::create(
            model_path,
            "",                      // config (empty for ONNX)
            cv::Size(input_w, input_h),
            confidence_threshold,
            0.3f,                    // NMS threshold
            5000                     // top_k
        );

        // Test that detect() actually works (OpenCV 4.6 may crash here)
        cv::Mat test_img = cv::Mat::zeros(input_h, input_w, CV_8UC3);
        cv::Mat test_faces;
        detector_->detect(test_img, test_faces);

        std::cout << "[face_detector] YuNet loaded OK (" << input_w << "x" << input_h << ")" << std::endl;
        return;
    } catch (const cv::Exception& e) {
        std::cerr << "[face_detector] YuNet failed (OpenCV " << CV_VERSION
                  << " incompatible): " << e.msg << std::endl;
        detector_.reset();
    }

    // Fall back to Haar cascade
    std::string haar_path = "/usr/share/opencv4/haarcascades/haarcascade_frontalface_default.xml";
    if (!haar_.load(haar_path)) {
        // Try alternative path
        haar_path = "/usr/share/opencv4/haarcascades/haarcascade_frontalface_alt2.xml";
        haar_.load(haar_path);
    }

    if (!haar_.empty()) {
        use_haar_ = true;
        std::cout << "[face_detector] Using Haar cascade fallback: " << haar_path << std::endl;
    } else {
        std::cerr << "[face_detector] No face detector available!" << std::endl;
    }
}

void FaceDetector::set_input_size(int w, int h) {
    if (detector_) {
        detector_->setInputSize(cv::Size(w, h));
    }
}

bool FaceDetector::detect(const cv::Mat& frame, FaceBox& best_face) {
    if (use_haar_) {
        // Haar cascade detection
        cv::Mat gray;
        cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
        cv::equalizeHist(gray, gray);

        std::vector<cv::Rect> faces;
        haar_.detectMultiScale(gray, faces, 1.1, 3, 0, cv::Size(60, 60));

        if (faces.empty()) return false;

        // Pick largest face
        int best_idx = 0;
        int best_area = 0;
        for (size_t i = 0; i < faces.size(); i++) {
            int area = faces[i].width * faces[i].height;
            if (area > best_area) {
                best_area = area;
                best_idx = static_cast<int>(i);
            }
        }

        best_face.x = faces[best_idx].x;
        best_face.y = faces[best_idx].y;
        best_face.w = faces[best_idx].width;
        best_face.h = faces[best_idx].height;
        best_face.confidence = 1.0f;
        return true;
    }

    // YuNet detection
    if (!detector_) return false;

    cv::Mat faces;
    try {
        detector_->detect(frame, faces);
    } catch (const cv::Exception& e) {
        std::cerr << "[face_detector] detect() error: " << e.what() << std::endl;
        return false;
    }

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
