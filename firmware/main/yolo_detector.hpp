#pragma once
#include <cstdint>
#include <list>

#include "dl_detect_base.hpp"
#include "dl_detect_yolo11_postprocessor.hpp"

// A YOLOv8/YOLO11 detector exported with ESP-DL's export script (outputs box0..2 and
// score0..2, box decoding left to the post-processor), loaded by name from a flash
// partition that holds several packed .espdl models.
class Yolo11Detector : public dl::detect::DetectImpl {
public:
    struct Timing {
        int64_t pre_us;
        int64_t model_us;
        int64_t post_us;
    };

    Yolo11Detector(const char *partition_label, const char *model_name, float score_thr, float nms_thr,
                   bool param_copy);

    // Same as DetectImpl::run, with the time of each stage.
    std::list<dl::detect::result_t> &run_timed(const dl::image::img_t &img, Timing &t);

    // Model input height and width (NHWC).
    int input_h();
    int input_w();
};
