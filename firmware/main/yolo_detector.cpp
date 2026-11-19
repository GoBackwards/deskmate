#include "yolo_detector.hpp"

#include "esp_timer.h"

Yolo11Detector::Yolo11Detector(const char *partition_label, const char *model_name, float score_thr,
                               float nms_thr, bool param_copy)
{
    m_model = new dl::Model(partition_label,
                            model_name,
                            fbs::MODEL_LOCATION_IN_FLASH_PARTITION,
                            0,
                            dl::MEMORY_MANAGER_GREEDY,
                            nullptr,
                            param_copy);
    m_model->minimize();
    // Ultralytics preprocessing: RGB scaled to [0, 1], letterboxed with grey 114.
    m_image_preprocessor = new dl::image::ImagePreprocessor(m_model, {0, 0, 0}, {255, 255, 255});
    m_image_preprocessor->enable_letterbox({114, 114, 114});
    m_postprocessor = new dl::detect::yolo11PostProcessor(
        m_model, m_image_preprocessor, score_thr, nms_thr, 10, {{8, 8, 4, 4}, {16, 16, 8, 8}, {32, 32, 16, 16}});
}

std::list<dl::detect::result_t> &Yolo11Detector::run_timed(const dl::image::img_t &img, Timing &t)
{
    const int64_t t0 = esp_timer_get_time();
    m_image_preprocessor->preprocess(img);
    const int64_t t1 = esp_timer_get_time();
    m_model->run();
    const int64_t t2 = esp_timer_get_time();
    m_postprocessor->clear_result();
    m_postprocessor->postprocess();
    std::list<dl::detect::result_t> &result = m_postprocessor->get_result(img.width, img.height);
    const int64_t t3 = esp_timer_get_time();
    t = {t1 - t0, t2 - t1, t3 - t2};
    return result;
}

int Yolo11Detector::input_h()
{
    return m_image_preprocessor->get_model_input()->shape[1];
}

int Yolo11Detector::input_w()
{
    return m_image_preprocessor->get_model_input()->shape[2];
}
