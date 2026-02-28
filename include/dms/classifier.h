#pragma once

#include <string>
#include <memory>
#include <utility>

namespace dms {

// TFLite INT8 drowsiness classifier
// Input: [ear_left, ear_right, mar]
// Output: (eye_closed_probability, yawn_probability)
class DrowsinessClassifier {
public:
    explicit DrowsinessClassifier(const std::string& model_path);
    ~DrowsinessClassifier();

    // Returns {eye_closed_prob, yawn_prob} in [0, 1]
    std::pair<float, float> predict(float ear_left, float ear_right, float mar);

    bool is_loaded() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dms
