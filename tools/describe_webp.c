/* Benchmark configuration audit; not part of the N.148i library.
 * Copyright (c) 2026 Micilini Roll. MIT License. */
#include <stdio.h>
#include <webp/encode.h>
#include <webp/decode.h>

#define FIELD(name) printf(",\"" #name "\":%.9g", (double)config.name)
int main(void) {
    const int qualities[] = {30, 45, 60, 75, 90};
    printf("{\"encoder_version\":%d,\"decoder_version\":%d,\"configurations\":[",
           WebPGetEncoderVersion(), WebPGetDecoderVersion());
    int first = 1;
    for (int method = 4; method <= 6; method += 2) {
        for (int point = 0; point < 5; point++) {
            WebPConfig config;
            if (!WebPConfigInit(&config)) return 1;
            config.lossless = 0;
            config.quality = (float)qualities[point];
            config.method = method;
            config.thread_level = 0;
            if (!WebPValidateConfig(&config)) return 1;
            printf("%s{\"input_use_argb\":0", first ? "" : ","); first = 0;
            FIELD(lossless); FIELD(quality); FIELD(method); FIELD(image_hint);
            FIELD(target_size); FIELD(target_PSNR); FIELD(segments);
            FIELD(sns_strength); FIELD(filter_strength); FIELD(filter_sharpness);
            FIELD(filter_type); FIELD(autofilter); FIELD(alpha_compression);
            FIELD(alpha_filtering); FIELD(alpha_quality); FIELD(pass);
            FIELD(show_compressed); FIELD(preprocessing); FIELD(partitions);
            FIELD(partition_limit); FIELD(emulate_jpeg_size); FIELD(thread_level);
            FIELD(low_memory); FIELD(near_lossless); FIELD(exact);
            FIELD(use_delta_palette); FIELD(use_sharp_yuv); FIELD(qmin); FIELD(qmax);
            printf("}");
        }
    }
    printf("]}\n");
    return 0;
}
