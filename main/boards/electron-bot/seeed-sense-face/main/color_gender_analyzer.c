/**
 * @file color_gender_analyzer.c
 * @brief 衣服颜色 & 性别粗判实现
 *
 * RGB888 → HSV 转换使用整数运算（不依赖 math.h），适合 ESP32-S3 运行。
 */

#include "color_gender_analyzer.h"

#include <stdint.h>
#include <string.h>
#include "esp_log.h"

#define TAG "ColorAnalyzer"

/* ─── RGB888 转 HSV（整数版，H:[0,179], S:[0,255], V:[0,255]） ─── */
static void rgb2hsv(uint8_t r, uint8_t g, uint8_t b,
                    int *h_out, int *s_out, int *v_out)
{
    int r_i = r, g_i = g, b_i = b;
    int vmax, vmin, diff;

    vmax = r_i;
    if (g_i > vmax) vmax = g_i;
    if (b_i > vmax) vmax = b_i;

    vmin = r_i;
    if (g_i < vmin) vmin = g_i;
    if (b_i < vmin) vmin = b_i;

    diff = vmax - vmin;

    *v_out = vmax;
    *s_out = (vmax == 0) ? 0 : (diff * 255 / vmax);

    if (diff == 0) {
        *h_out = 0;
        return;
    }

    int h;
    if (vmax == r_i) {
        /* h in [0,60) or [300,360) -> map to [0,30) or [150,179] */
        h = 30 * (g_i - b_i) / diff;
        if (h < 0) h += 180;
    } else if (vmax == g_i) {
        h = 30 * (b_i - r_i) / diff + 60;
    } else {
        h = 30 * (r_i - g_i) / diff + 120;
    }
    /* clamp */
    if (h < 0)   h = 0;
    if (h > 179) h = 179;
    *h_out = h;
}

/* ─── HSV → 颜色枚举 ─── */
static cloth_color_t hsv_to_color(int h, int s, int v)
{
    /* 黑色：非常暗 */
    if (v < 50)  return COLOR_BLACK;

    /* 白色：低饱和度 + 高亮度 */
    if (s < 40 && v > 180) return COLOR_WHITE;

    /* 灰色：低饱和度 */
    if (s < 50)  return COLOR_GRAY;

    /* 彩色：按色相 */
    if (h <= 10 || h >= 160) return COLOR_RED;
    if (h <= 25)             return COLOR_ORANGE;
    if (h <= 34)             return COLOR_YELLOW;
    if (h <= 85)             return COLOR_GREEN;
    if (h <= 99)             return COLOR_CYAN;
    if (h <= 130)            return COLOR_BLUE;
    /* h <= 159 */           return COLOR_PURPLE;
}

/* ─── 分析衣服主色 ─── */
cloth_color_t analyze_cloth_color(const uint8_t *rgb_buf,
                                   int img_w, int img_h,
                                   int face_x1, int face_y1,
                                   int face_x2, int face_y2)
{
    /* 人脸框尺寸 */
    int face_w = face_x2 - face_x1;
    int face_h = face_y2 - face_y1;
    if (face_w <= 0 || face_h <= 0) return COLOR_UNKNOWN;

    /*
     * 衣服采样区域：
     *   - X 方向：与人脸框同宽，两侧各缩进 10%（避免背景）
     *   - Y 方向：从人脸框底部开始，向下取 0.8 ~ 1.8 倍人脸高度
     *     （跳过脖子，直接取胸部衣领区域）
     */
    int margin_x = face_w / 10;
    int roi_x1   = face_x1 + margin_x;
    int roi_x2   = face_x2 - margin_x;
    /* 衣服区域：人脸框正下方。先跳过一小段脖子(0.3*脸高)，向下取最多1.8倍脸高 */
    int roi_y1   = face_y2 + (int)(face_h * 0.3f);
    int roi_y2   = face_y2 + (int)(face_h * 1.8f);

    /* 边界裁剪（上下都要裁，避免越界导致 ROI 失效） */
    if (roi_x1 < 0)      roi_x1 = 0;
    if (roi_x2 >= img_w) roi_x2 = img_w - 1;
    if (roi_y1 < 0)      roi_y1 = 0;
    if (roi_y1 >= img_h) roi_y1 = img_h - 1;
    if (roi_y2 < 0)      roi_y2 = 0;
    if (roi_y2 >= img_h) roi_y2 = img_h - 1;

    /* 若偏移后的衣服带仍无效（脸太大/太靠下，下方无空间），
       退回到"人脸框正下方到画面底部"这一整段 */
    if (roi_y1 >= roi_y2) {
        roi_y1 = face_y2;
        roi_y2 = img_h - 1;
    }

    if (roi_x1 >= roi_x2 || roi_y1 >= roi_y2) {
        ESP_LOGW(TAG, "Cloth ROI out of frame (%d,%d)-(%d,%d)", roi_x1, roi_y1, roi_x2, roi_y2);
        return COLOR_UNKNOWN;
    }

    /* 统计各颜色票数（步长 4 抽样，降低计算量） */
    int votes[COLOR_UNKNOWN + 1] = {0};
    int step = 4;

    for (int y = roi_y1; y < roi_y2; y += step) {
        for (int x = roi_x1; x < roi_x2; x += step) {
            int idx = (y * img_w + x) * 3;
            uint8_t rr = rgb_buf[idx];
            uint8_t gg = rgb_buf[idx + 1];
            uint8_t bb = rgb_buf[idx + 2];
            int h, s, v;
            rgb2hsv(rr, gg, bb, &h, &s, &v);
            cloth_color_t c = hsv_to_color(h, s, v);
            votes[c]++;
        }
    }

    /* 找票数最多的颜色（排除 UNKNOWN，但如全是 UNKNOWN 则返回） */
    int best       = COLOR_UNKNOWN;
    int best_votes = 0;
    for (int i = 0; i < COLOR_UNKNOWN; i++) {
        if (votes[i] > best_votes) {
            best_votes = votes[i];
            best       = i;
        }
    }

    ESP_LOGD(TAG, "Cloth ROI (%d,%d)-(%d,%d) best=%d votes=%d",
             roi_x1, roi_y1, roi_x2, roi_y2, best, best_votes);

    return (cloth_color_t)best;
}

/* ─── 性别粗判 ─── */
gender_t estimate_gender(int face_x1, int face_y1, int face_x2, int face_y2)
{
    int face_w = face_x2 - face_x1;
    int face_h = face_y2 - face_y1;
    if (face_h <= 0) return GENDER_UNKNOWN;

    /*
     * 宽高比经验阈值：
     *   男性脸型更宽方，ratio 较大
     *   女性脸型偏瘦长，ratio 较小
     *   阈值 0.82：在小分辨率 QVGA 下实测调优
     */
    float ratio = (float)face_w / (float)face_h;
    if (ratio >= 0.82f) {
        return GENDER_MALE;
    } else {
        return GENDER_FEMALE;
    }
}

/* ─── 转换函数 ─── */
const char *color_to_chinese(cloth_color_t color)
{
    switch (color) {
        case COLOR_RED:     return "红色";
        case COLOR_ORANGE:  return "橙色";
        case COLOR_YELLOW:  return "黄色";
        case COLOR_GREEN:   return "绿色";
        case COLOR_CYAN:    return "青色";
        case COLOR_BLUE:    return "蓝色";
        case COLOR_PURPLE:  return "紫色";
        case COLOR_WHITE:   return "白色";
        case COLOR_BLACK:   return "黑色";
        case COLOR_GRAY:    return "灰色";
        default:            return "彩色";
    }
}

const char *gender_to_chinese(gender_t gender)
{
    switch (gender) {
        case GENDER_MALE:   return "小哥";
        case GENDER_FEMALE: return "美女";
        default:            return "朋友";
    }
}
