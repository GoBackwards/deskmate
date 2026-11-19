#include "pad_classifier.hpp"

#include <cmath>

#include "dl_define.hpp"

PadClassifier::PadClassifier(const char *partition_label, const char *model_name)
{
    m_model = new dl::Model(partition_label,
                            model_name,
                            fbs::MODEL_LOCATION_IN_FLASH_PARTITION,
                            0,
                            dl::MEMORY_MANAGER_GREEDY,
                            nullptr,
                            true);
    m_model->minimize();
    // Same as training: RGB scaled to [0, 1], crop stretched to the input size (no letterbox).
    m_pre = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255});
}

PadClassifier::~PadClassifier()
{
    delete m_pre;
    delete m_model;
}

static float read_value(dl::TensorBase *t, int i)
{
    switch (t->dtype) {
    case dl::DATA_TYPE_INT8:
        return ((int8_t *)t->data)[i] * DL_SCALE(t->exponent);
    case dl::DATA_TYPE_INT16:
        return ((int16_t *)t->data)[i] * DL_SCALE(t->exponent);
    case dl::DATA_TYPE_FLOAT:
        return ((float *)t->data)[i];
    default:
        return 0.0f;
    }
}

float PadClassifier::run(const dl::image::img_t &img, const std::vector<int> &roi)
{
    m_pre->preprocess(img, roi);
    m_model->run();
    dl::TensorBase *out = m_model->get_output();
    const float l0 = read_value(out, 0);  // no phone
    const float l1 = read_value(out, 1);  // phone
    return 1.0f / (1.0f + expf(l0 - l1));
}

int PadClassifier::input_h()
{
    return m_pre->get_model_input()->shape[1];
}

int PadClassifier::input_w()
{
    return m_pre->get_model_input()->shape[2];
}
