#pragma once
#include <vector>

#include "dl_image_preprocessor.hpp"
#include "dl_model_base.hpp"

// "Is the phone on the pad?" CNN (trained in ml/train_pad.py). The pad region of the
// camera frame is cropped and resized to the model input by ESP-DL's preprocessor;
// the model outputs two logits: [no phone, phone].
class PadClassifier {
public:
    PadClassifier(const char *partition_label, const char *model_name);
    ~PadClassifier();
    // roi = {x1, y1, x2, y2} in frame pixels. Returns p(phone on pad) in [0, 1].
    float run(const dl::image::img_t &img, const std::vector<int> &roi);
    int input_w();
    int input_h();

private:
    dl::Model *m_model;
    dl::image::ImagePreprocessor *m_pre;
};
