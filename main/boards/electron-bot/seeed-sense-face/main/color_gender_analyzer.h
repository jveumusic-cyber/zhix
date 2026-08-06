#pragma once

/**
 * @file color_gender_analyzer.h
 * @brief 衣服颜色 & 性别粗判分析模块
 *
 * 基于人脸 ROI 下方区域的 RGB888 像素做 HSV 颜色统计，
 * 输出主色名（中文）和粗判性别。
 *
 * 颜色映射（OpenCV HSV 范围，H:[0,179] S:[0,255] V:[0,255]）：
 *   红色   H ∈ [0,10] ∪ [160,179], S>80, V>50
 *   橙色   H ∈ [11,25], S>80, V>50
 *   黄色   H ∈ [26,34], S>80, V>80
 *   绿色   H ∈ [35,85], S>60, V>40
 *   青色   H ∈ [86,99], S>60, V>40
 *   蓝色   H ∈ [100,130], S>60, V>40
 *   紫色   H ∈ [131,159], S>60, V>40
 *   白色   S<40 && V>180
 *   黑色   V<50
 *   灰色   其余
 *
 * 性别粗判：
 *   人脸宽高比 < 0.85 → 女（脸型偏瘦长）
 *   人脸宽高比 >= 0.85 → 男（脸型偏宽方）
 *   注意：这只是一个粗略的启发式判断，准确率有限。
 */

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 颜色枚举 */
typedef enum {
    COLOR_RED    = 0,
    COLOR_ORANGE = 1,
    COLOR_YELLOW = 2,
    COLOR_GREEN  = 3,
    COLOR_CYAN   = 4,
    COLOR_BLUE   = 5,
    COLOR_PURPLE = 6,
    COLOR_WHITE  = 7,
    COLOR_BLACK  = 8,
    COLOR_GRAY   = 9,
    COLOR_UNKNOWN= 10,
} cloth_color_t;

/** 性别枚举 */
typedef enum {
    GENDER_MALE   = 0,
    GENDER_FEMALE = 1,
    GENDER_UNKNOWN= 2,
} gender_t;

/**
 * @brief 分析分析区域衣服主色（纯 C，不依赖 esp-dl）
 *
 * @param rgb_buf   RGB888 图像缓冲（row-major, R G B R G B ...）
 * @param img_w     图像宽度（像素）
 * @param img_h     图像高度（像素）
 * @param face_x1   人脸框左上角 X
 * @param face_y1   人脸框左上角 Y
 * @param face_x2   人脸框右下角 X
 * @param face_y2   人脸框右下角 Y
 * @return 主色枚举
 */
cloth_color_t analyze_cloth_color(const uint8_t *rgb_buf,
                                   int img_w, int img_h,
                                   int face_x1, int face_y1,
                                   int face_x2, int face_y2);

/**
 * @brief 根据人脸宽高比粗判性别
 *
 * @param face_x1  人脸框左上角 X
 * @param face_y1  人脸框左上角 Y
 * @param face_x2  人脸框右下角 X
 * @param face_y2  人脸框右下角 Y
 * @return 性别枚举
 */
gender_t estimate_gender(int face_x1, int face_y1, int face_x2, int face_y2);

/** 将颜色枚举转换为中文名称 */
const char *color_to_chinese(cloth_color_t color);

/** 将性别枚举转换为中文称谓 */
const char *gender_to_chinese(gender_t gender);

#ifdef __cplusplus
}
#endif
