// Copyright (c) 2021 by Rockchip Electronics Co., Ltd. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

/*-------------------------------------------
                Includes
-------------------------------------------*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <dlfcn.h>
#include <vector>

#define _BASETSD_H

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb/stb_image_resize.h>

#undef cimg_display
#define cimg_display 0
#include "CImg/CImg.h"

#include "rga_func.h"
#include "rknn_api.h"
#include "postprocess.h"

#define PERF_WITH_POST 1

using namespace cimg_library;

/*-------------------------------------------
                  Functions
-------------------------------------------*/

inline const char *get_type_string(rknn_tensor_type type)
{
    switch (type)
    {
    case RKNN_TENSOR_FLOAT32:
        return "FP32";
    case RKNN_TENSOR_FLOAT16:
        return "FP16";
    case RKNN_TENSOR_INT8:
        return "INT8";
    case RKNN_TENSOR_UINT8:
        return "UINT8";
    case RKNN_TENSOR_INT16:
        return "INT16";
    default:
        return "UNKNOW";
    }
}

inline const char *get_qnt_type_string(rknn_tensor_qnt_type type)
{
    switch (type)
    {
    case RKNN_TENSOR_QNT_NONE:
        return "NONE";
    case RKNN_TENSOR_QNT_DFP:
        return "DFP";
    case RKNN_TENSOR_QNT_AFFINE_ASYMMETRIC:
        return "AFFINE";
    default:
        return "UNKNOW";
    }
}

inline const char *get_format_string(rknn_tensor_format fmt)
{
    switch (fmt)
    {
    case RKNN_TENSOR_NCHW:
        return "NCHW";
    case RKNN_TENSOR_NHWC:
        return "NHWC";
    default:
        return "UNKNOW";
    }
}

static void dump_tensor_attr(rknn_tensor_attr *attr)
{
    printf("  index=%d, name=%s, n_dims=%d, dims=[%d, %d, %d, %d], n_elems=%d, size=%d, fmt=%s, type=%s, qnt_type=%s, "
           "zp=%d, scale=%f\n",
           attr->index, attr->name, attr->n_dims, attr->dims[3], attr->dims[2], attr->dims[1], attr->dims[0],
           attr->n_elems, attr->size, get_format_string(attr->fmt), get_type_string(attr->type),
           get_qnt_type_string(attr->qnt_type), attr->zp, attr->scale);
}

double __get_us(struct timeval t) { return (t.tv_sec * 1000000 + t.tv_usec); }

static unsigned char *load_data(FILE *fp, size_t ofst, size_t sz)
{
    unsigned char *data;
    int ret;

    data = NULL;

    if (NULL == fp)
    {
        return NULL;
    }

    ret = fseek(fp, ofst, SEEK_SET);
    if (ret != 0)
    {
        printf("blob seek failure.\n");
        return NULL;
    }

    data = (unsigned char *)malloc(sz);
    if (data == NULL)
    {
        printf("buffer malloc failure.\n");
        return NULL;
    }
    ret = fread(data, 1, sz, fp);
    return data;
}

static unsigned char *load_model(const char *filename, int *model_size)
{

    FILE *fp;
    unsigned char *data;

    fp = fopen(filename, "rb");
    if (NULL == fp)
    {
        printf("Open file %s failed.\n", filename);
        return NULL;
    }

    fseek(fp, 0, SEEK_END);
    int size = ftell(fp);

    data = load_data(fp, 0, size);

    fclose(fp);

    *model_size = size;
    return data;
}

static int saveFloat(const char *file_name, float *output, int element_size)
{
    FILE *fp;
    fp = fopen(file_name, "w");
    for (int i = 0; i < element_size; i++)
    {
        fprintf(fp, "%.6f\n", output[i]);
    }
    fclose(fp);
    return 0;
}

static unsigned char *load_image(const char *image_path, int *org_height, int *org_width, int *org_ch, rknn_tensor_attr *input_attr)
{
    int req_height = 0;
    int req_width = 0;
    int req_channel = 0;

    switch (input_attr->fmt)
    {
    case RKNN_TENSOR_NHWC:
        req_height = input_attr->dims[2];
        req_width = input_attr->dims[1];
        req_channel = input_attr->dims[0];
        break;
    case RKNN_TENSOR_NCHW:
        req_height = input_attr->dims[1];
        req_width = input_attr->dims[0];
        req_channel = input_attr->dims[2];
        break;
    default:
        printf("meet unsupported layout\n");
        return NULL;
    }

    int height = 0;
    int width = 0;
    int channel = 0;

    unsigned char *image_data = stbi_load(image_path, &width, &height, &channel, req_channel);
    if (image_data == NULL)
    {
        printf("load image failed!\n");
        return NULL;
    }
    *org_width = width;
    *org_height = height;
    *org_ch = channel;

    return image_data;
}


/*-------------------------------------------
                  Main Functions
-------------------------------------------*/
int main(int argc, char **argv)
{
    int status = 0;
    char *model_name = NULL;
    rknn_context ctx;
    int is_zerocopy = 0;
    int img_width = 0;
    int img_height = 0;
    int img_channel = 0;
    void *resize_buf = NULL;
    rga_context rga_ctx;
    const float nms_threshold = NMS_THRESH;
    const float box_conf_threshold = BOX_THRESH;
    struct timeval start_time, stop_time;
    int ret;

    memset(&rga_ctx, 0, sizeof(rga_context));

    if (argc != 4)
    {
        printf("Note: rknn model need meet zero-copy condition: 3-channel with same integer means and same scales\n");
        printf("Usage: %s <rknn model> <bmp> <flag>\n", argv[0]);
        printf("flag:\n");
        printf("\t 0: run builtin_permute=False rknn model, perform rknn_inputs_set\n");
        printf("\t 1: run builtin_permute=True rknn model, perform rknn_set_io_mem\n");
        return -1;
    }

    printf("post process config: box_conf_threshold = %.2f, nms_threshold = %.2f\n",
           box_conf_threshold, nms_threshold);
    model_name = (char *)argv[1];
    char *image_name = argv[2];
    is_zerocopy = atoi(argv[3]);

    // if (strstr(image_name, ".jpg") != NULL || strstr(image_name, ".png") != NULL)
    // {
    //     printf("Error: read %s failed! only support .bmp format image\n", image_name);
    //     return -1;
    // }

    /* Create the neural network */
    printf("Loading mode...\n");
    int model_data_size = 0;
    unsigned char *model_data = load_model(model_name, &model_data_size);
    ret = rknn_init(&ctx, model_data, model_data_size, 0);
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }

    rknn_sdk_version version;
    ret = rknn_query(ctx, RKNN_QUERY_SDK_VERSION, &version,
                     sizeof(rknn_sdk_version));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }
    printf("sdk version: %s driver version: %s\n", version.api_version,
           version.drv_version);

    rknn_input_output_num io_num;
    ret = rknn_query(ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num));
    if (ret < 0)
    {
        printf("rknn_init error ret=%d\n", ret);
        return -1;
    }
    printf("model input num: %d, output num: %d\n", io_num.n_input,
           io_num.n_output);

    rknn_tensor_attr input_attrs[io_num.n_input];
    memset(input_attrs, 0, sizeof(input_attrs));
    for (int i = 0; i < io_num.n_input; i++)
    {
        input_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_INPUT_ATTR, &(input_attrs[i]),
                         sizeof(rknn_tensor_attr));
        if (ret < 0)
        {
            printf("rknn_init error ret=%d\n", ret);
            return -1;
        }
        dump_tensor_attr(&(input_attrs[i]));
    }

    rknn_tensor_attr output_attrs[io_num.n_output];
    memset(output_attrs, 0, sizeof(output_attrs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        output_attrs[i].index = i;
        ret = rknn_query(ctx, RKNN_QUERY_OUTPUT_ATTR, &(output_attrs[i]),
                         sizeof(rknn_tensor_attr));
        dump_tensor_attr(&(output_attrs[i]));
    }

    int channel = 3;
    int width = 0;
    int height = 0;
    if (input_attrs[0].fmt == RKNN_TENSOR_NCHW)
    {
        printf("model is NCHW input fmt\n");
        width = input_attrs[0].dims[0];
        height = input_attrs[0].dims[1];
    }
    else
    {
        printf("model is NHWC input fmt\n");
        width = input_attrs[0].dims[1];
        height = input_attrs[0].dims[2];
    }

    printf("model input height=%d, width=%d, channel=%d\n", height, width,
           channel);

    // Load image
    CImg<unsigned char> img(image_name);
    unsigned char *input_data = NULL;
    input_data = load_image(image_name, &img_height, &img_width, &img_channel, &input_attrs[0]);
    if (!input_data)
    {
        return -1;
    }

    // for non zero copy
    rknn_input inputs[1];
    rknn_output outputs[io_num.n_output];

    memset(inputs, 0, sizeof(inputs));    
    memset(outputs, 0, sizeof(outputs));
    for (int i = 0; i < io_num.n_output; i++)
    {
        outputs[i].want_float = 0;
    }

    // for zero copy
    rknn_tensor_mem* inputs_mem[io_num.n_input];
    rknn_tensor_mem* outputs_mem[io_num.n_output];

    for (int i = 0; i < io_num.n_input; i++) {
        inputs_mem[i] = rknn_create_mem(ctx, input_attrs[i].size);
    }

    for (int i = 0; i < io_num.n_output; i++)
    {
        outputs_mem[i] = rknn_create_mem(ctx, output_attrs[i].size);
    }   

    // init rga context
    RGA_init(&rga_ctx);
    if (is_zerocopy == 0)
    {
        void *resize_buf = malloc(height * width * channel);

        rga_resize(&rga_ctx, -1, input_data, img_width, img_height, -1, resize_buf, width, height);
        
        inputs[0].index = 0;
        inputs[0].type = RKNN_TENSOR_UINT8;
        inputs[0].size = width * height * channel;
        inputs[0].fmt = RKNN_TENSOR_NHWC;
        inputs[0].pass_through = 0;
        inputs[0].buf = resize_buf;

        gettimeofday(&start_time, NULL);
        rknn_inputs_set(ctx, io_num.n_input, inputs);
    }
    else
    {
        rga_resize(&rga_ctx, -1, input_data, img_width, img_height, inputs_mem[0]->fd, nullptr, width, height);

        gettimeofday(&start_time, NULL);
        rknn_set_io_mem(ctx, inputs_mem[0], &input_attrs[0]);

        for (int i = 0; i < io_num.n_output; i++)
        {
            rknn_set_io_mem(ctx, outputs_mem[i], &output_attrs[i]);
        } 

            gettimeofday(&stop_time, NULL);
    printf("rknn_set_io_mem use %f ms\n",
           (__get_us(stop_time) - __get_us(start_time)) / 1000);
    }

    ret = rknn_run(ctx, NULL);

    gettimeofday(&stop_time, NULL);
    printf("once run use %f ms\n",
           (__get_us(stop_time) - __get_us(start_time)) / 1000);

    if (is_zerocopy == 0) {
        ret = rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
    }
    
    //post process
    float scale_w = (float)width / img_width;
    float scale_h = (float)height / img_height;

    detect_result_group_t detect_result_group;
    std::vector<float> out_scales;
    std::vector<uint32_t> out_zps;
    for (int i = 0; i < io_num.n_output; ++i)
    {
        out_scales.push_back(output_attrs[i].scale);
        out_zps.push_back(output_attrs[i].zp);
    }

    if (is_zerocopy == 0) {
        post_process((uint8_t *)outputs[0].buf, (uint8_t *)outputs[1].buf, (uint8_t *)outputs[2].buf, height, width,
                 box_conf_threshold, nms_threshold, scale_w, scale_h, out_zps, out_scales, &detect_result_group);
    } else {
        post_process((uint8_t *)outputs_mem[0]->logical_addr, (uint8_t *)outputs_mem[1]->logical_addr, (uint8_t *)outputs_mem[2]->logical_addr, height, width,
                 box_conf_threshold, nms_threshold, scale_w, scale_h, out_zps, out_scales, &detect_result_group);
    }

    // Draw Objects
    char text[256];
    const unsigned char blue[] = {0, 0, 255};
    const unsigned char white[] = {255, 255, 255};
    for (int i = 0; i < detect_result_group.count; i++)
    {
        detect_result_t *det_result = &(detect_result_group.results[i]);
        sprintf(text, "%s %.2f", det_result->name, det_result->prop);
        printf("%s @ (%d %d %d %d) %f\n",
               det_result->name,
               det_result->box.left, det_result->box.top, det_result->box.right, det_result->box.bottom,
               det_result->prop);
        int x1 = det_result->box.left;
        int y1 = det_result->box.top;
        int x2 = det_result->box.right;
        int y2 = det_result->box.bottom;
        //draw box
        img.draw_rectangle(x1, y1, x2, y2, blue, 1, ~0U);
        img.draw_text(x1, y1 - 12, text, white);
    }
    img.save("./out.bmp");

    if (is_zerocopy == 0) {
        ret = rknn_outputs_release(ctx, io_num.n_output, outputs);
    }

    // loop test
    int test_count = 100;
    gettimeofday(&start_time, NULL);
    if (is_zerocopy == 0)
    {
        for (int i = 0; i < test_count; ++i)
        {
            rknn_inputs_set(ctx, io_num.n_input, inputs);
            ret = rknn_run(ctx, NULL);
            ret = rknn_outputs_get(ctx, io_num.n_output, outputs, NULL);
#if PERF_WITH_POST
            post_process((uint8_t *)outputs[0].buf, (uint8_t *)outputs[1].buf, (uint8_t *)outputs[2].buf, height, width,
                         box_conf_threshold, nms_threshold, scale_w, scale_h, out_zps, out_scales, &detect_result_group);
#endif
            ret = rknn_outputs_release(ctx, io_num.n_output, outputs);
        }
    }
    else
    {
        for (int i = 0; i < test_count; ++i)
        {
            rknn_set_io_mem(ctx, inputs_mem[0], &input_attrs[0]);

            // for (int i = 0; i < io_num.n_output; i++)
            // {
            //     rknn_set_io_mem(ctx, outputs_mem[i], &output_attrs[i]);
            // } 

            ret = rknn_run(ctx, NULL);
#if PERF_WITH_POST
            post_process((uint8_t *)outputs_mem[0]->logical_addr, (uint8_t *)outputs_mem[1]->logical_addr, (uint8_t *)outputs_mem[2]->logical_addr, height, width,
                        box_conf_threshold, nms_threshold, scale_w, scale_h, out_zps, out_scales, &detect_result_group);
#endif
        }
    }
    gettimeofday(&stop_time, NULL);
    printf("run loop count = %d , average time: %f ms\n", test_count,
           (__get_us(stop_time) - __get_us(start_time)) / 1000.0 / test_count);

    // release
    ret = rknn_destroy(ctx);

    for (int i = 0; i < io_num.n_input; i++)
    {
        rknn_destroy_mem(ctx, inputs_mem[i]);
    }

    for (int i = 0; i < io_num.n_output; i++)
    {
        rknn_destroy_mem(ctx, outputs_mem[i]);
    }   

    RGA_deinit(&rga_ctx);
    if (model_data)
    {
        free(model_data);
    }

    if (resize_buf)
    {
        free(resize_buf);
    }

    return 0;
}
