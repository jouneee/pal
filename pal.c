#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <limits.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define SAMPLE_COUNT 1024
#define MAX_PLACEHOLDER_LEN 32

typedef struct {
    uint8_t r, g, b;
    int vibrancy;
    float luminance;
} Color;

typedef enum {
    AAVERAGE = 0,
    KMEANS = 1,
} Method;

typedef enum {
    RGB = 0,
    HEX = 1,
} Format;

typedef struct {
    int bgfg;
    int num_accents;
    float saturation;
    Method method;
    Format format;
} Config;

float get_luminance(Color c) {
    return (0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b) / 255.0f;
}

float color_dist(Color a, Color b) {
    return sqrtf(powf(a.r - b.r, 2) + powf(a.g - b.g, 2) + powf(a.b - b.b, 2));
}

int calculate_vibrancy(Color c) {
    int max = c.r > c.g ? (c.r > c.b ? c.r : c.b) : (c.g > c.b ? c.g : c.b);
    int min = c.r < c.g ? (c.r < c.b ? c.r : c.b) : (c.g < c.b ? c.g : c.b);
    return max - min;
}

int compare_vibrancy(const void *a, const void *b) {
    return ((Color*)b)->vibrancy - ((Color*)a)->vibrancy;
}

uint8_t clamp_u8(float v) {
    return (v < 0) ? 0 : (v > 255) ? 255 : (uint8_t)v;
}

void generate_scheme(uint8_t *pixels, int w, int h, Config config, Color *out_palette, Color *out_bg, Color *out_fg) {
    int divisor = 32;
    int count = 0;
    Color samples[SAMPLE_COUNT];

    int step_y = (h / divisor) > 0 ? (h / divisor) : 1;
    int step_x = (w / divisor) > 0 ? (w / divisor) : 1;

    Color darkest = {255, 255, 255, 0, 1.0f};
    Color lightest = {0, 0, 0, 0, 0.0f};

    for (int y = 0; y < h && count < SAMPLE_COUNT; y += step_y) {
        for (int x = 0; x < w && count < SAMPLE_COUNT; x += step_x) {
            float r_acc = 0, g_acc = 0, b_acc = 0;
            for (int ky = 0; ky < 4; ky++) {
                for (int kx = 0; kx < 4; kx++) {
                    size_t idx = ((y + ky) * w + (x + kx)) * 4;
                    r_acc += pixels[idx];
                    g_acc += pixels[idx+1];    
                    b_acc += pixels[idx+2];
                }
            }
            
            Color c;
            c.r = (uint8_t)(r_acc / 16.0f);
            c.g = (uint8_t)(g_acc / 16.0f);
            c.b = (uint8_t)(b_acc / 16.0f);
            c.vibrancy = calculate_vibrancy(c);
            c.luminance = get_luminance(c);
            if (c.luminance < darkest.luminance && c.luminance > 0.05f) darkest = c;
            if (c.luminance > lightest.luminance && c.luminance < 0.95f) lightest = c;

            samples[count++] = c;
        }
    }

    *out_bg = darkest;
    *out_fg = lightest;
    qsort(samples, count, sizeof(Color), compare_vibrancy);

    int picked = 0;
    Color selected[16];
    for (int i = 0; i < count && picked < config.num_accents; i++) {
        int distinct = 1;
        
        // dont pick colors too close to bg or fg
        float diff_bg = fabsf(samples[i].luminance - darkest.luminance);
        float diff_fg = fabsf(samples[i].luminance - lightest.luminance);
        if (diff_bg < 0.15f || diff_fg < 0.15f) distinct = 0;

        for (int j = 0; j < picked; j++) {
            int d = abs(samples[i].r - selected[j].r) + 
                    abs(samples[i].g - selected[j].g) + 
                    abs(samples[i].b - selected[j].b);
            if (d < 50) { distinct = 0; break; }
        }

        if (distinct) {
            selected[picked++] = samples[i];
        }
    }

    for (int k = 0; k < picked; k++) {
        Color adjusted = selected[k];
        if (config.saturation != 1.0f) {
            float gray = adjusted.luminance * 255.0f;
            adjusted.r = clamp_u8(gray + config.saturation * (adjusted.r - gray));
            adjusted.g = clamp_u8(gray + config.saturation * (adjusted.g - gray));
            adjusted.b = clamp_u8(gray + config.saturation * (adjusted.b - gray));
        }
        out_palette[k] = adjusted;
    }
}

void generate_scheme_kmeans(uint8_t *pixels, int w, int h, Config config, Color *out_palette, Color *out_bg, Color *out_fg) {
    int divisor = 32;
    int count = 0;
    Color samples[SAMPLE_COUNT];

    int step_y = (h / divisor) > 0 ? (h / divisor) : 1;
    int step_x = (w / divisor) > 0 ? (w / divisor) : 1;

    Color darkest = {255, 255, 255, 0, 1.0f};
    Color lightest = {0, 0, 0, 0, 0.0f};

    for (int y = 0; y < h && count < SAMPLE_COUNT; y += step_y) {
        for (int x = 0; x < w && count < SAMPLE_COUNT; x += step_x) {
            size_t idx = (y * w + x) * 4;
            Color c;
            c.r = pixels[idx];
            c.g = pixels[idx+1];
            c.b = pixels[idx+2];
            c.vibrancy = calculate_vibrancy(c);
            c.luminance = get_luminance(c);

            if (c.luminance < darkest.luminance && c.luminance > 0.05f) {
                darkest = c;
            }
            if (c.luminance > lightest.luminance && c.luminance < 0.95f) {
                lightest = c;
            }
            samples[count++] = c;
        }
    } 

    *out_bg = darkest;
    *out_fg = lightest;
    qsort(samples, count, sizeof(Color), compare_vibrancy);

    Color centers[16];
    for (int i = 0; i < 16; i++) centers[i] = samples[i * 64];

    for (int iter = 0; iter < 10; iter++) {
        long r_sum[16] = {0},g_sum[16] = {0},b_sum[16] = {0};
        int counts[16] = {0};
        
        for (int idx = 0; idx < SAMPLE_COUNT; idx++) {
            int best_idx = 0;
            float best_dist = 1e10;

            for (int k = 0; k < 16; k++) {
                float d = color_dist(samples[idx], centers[k]);
                if (d < best_dist) {
                    best_dist = d;
                    best_idx = k;
                }
            }
            
            r_sum[best_idx] += samples[idx].r;
            g_sum[best_idx] += samples[idx].g; 
            b_sum[best_idx] += samples[idx].b;
            counts[best_idx]++;
        }
        for (int k = 0; k < config.num_accents; k++) {    
            if (counts[k] > 0) {
                centers[k].r = (uint8_t)(r_sum[k] / counts[k]);
                centers[k].g = (uint8_t)(g_sum[k] / counts[k]);
                centers[k].b = (uint8_t)(b_sum[k] / counts[k]);
                centers[k].luminance = get_luminance(centers[k]);
                centers[k].vibrancy = calculate_vibrancy(centers[k]);
            }
        }
    }
    for (int k = 0; k < config.num_accents; k++) {
        Color adjusted = centers[k];

        if (config.saturation != 1.0f) {
            float gray = adjusted.luminance * 255.0f;
            adjusted.r = clamp_u8(gray + config.saturation * (adjusted.r - gray));
            adjusted.g = clamp_u8(gray + config.saturation * (adjusted.g - gray));
            adjusted.b = clamp_u8(gray + config.saturation * (adjusted.b - gray));
        }

        out_palette[k] = adjusted;
    }
}

static void color_to_string(Color c, Format format, char *buf, size_t buflen) {
    if (format == RGB) {
        snprintf(buf, buflen, "rgb(%d, %d, %d)", c.r, c.g, c.b);
    } else {
        snprintf(buf, buflen, "#%02X%02X%02X", c.r, c.g, c.b);
    }
}

void template_processor(FILE *template_file, Color bg, Color fg, Color *palette, Config config) {
    
    char bg_str[20], fg_str[20];
    color_to_string(bg, config.format, bg_str, sizeof(bg_str));
    color_to_string(fg, config.format, fg_str, sizeof(fg_str));

    char **accent_strings = malloc(config.num_accents * sizeof(char*));
    for (int i = 0; i < config.num_accents; i++) {
        accent_strings[i] = malloc(20);
        color_to_string(palette[i], config.format, accent_strings[i], 20);
    }
    
    enum { OUTSIDE, INSIDE } state = OUTSIDE;
    char buffer[MAX_PLACEHOLDER_LEN];
    int buf_pos = 0;

    int c;
    while ((c = fgetc(template_file)) != EOF) {
        switch (state) {
            case OUTSIDE:
                if (c == '`') {
                    state = INSIDE;
                    buf_pos = 0;
                } else {
                    putchar(c);
                }
                break;
            case INSIDE:
                if (c == '`') {
                    buffer[buf_pos] = '\0';

                    if (buffer[0] == '@') {
                        if (strcmp(buffer, "@background") == 0) {
                            fputs(bg_str, stdout);
                        } else if (strcmp(buffer, "@foreground") == 0) {
                            fputs(fg_str, stdout);
                        } else if (strncmp(buffer, "@color", 6) == 0) {
                            int i = atoi(buffer + 6);
                            if (i >= 0 && i < config.num_accents) {
                                fputs(accent_strings[i], stdout);
                            } else {
                                printf("`%s`", buffer); // invalid string will be spat back out
                            }
                        } else {
                            printf("`%s`", buffer);
                        }
                    }
                    state = OUTSIDE;
                }
                else if (buf_pos < MAX_PLACEHOLDER_LEN - 1) {
                    buffer[buf_pos++] = c;              // silently drop placeholders too long
                }
                break;
        }
    }
    if (state == INSIDE) {
        fprintf(stderr, "Warning: unterminated placeholder at EOF\n");
        printf("`%.*s", MAX_PLACEHOLDER_LEN - 1, buffer);
    }

    for (int i = 0; i < config.num_accents; i++) {
        free(accent_strings[i]);
    }
    free(accent_strings);
}


uint32_t fnv32_hash(unsigned char *s, size_t len)
{
    const uint32_t FNV_32_PRIME = 0x01000193;
    uint32_t h = 0x811c9dc5;
    while (len--) {
        h ^= *s++;
        h *= FNV_32_PRIME;
    }
    return h;
}

uint32_t final_hash(uint8_t *pixels, size_t len, Config config) 
{
    uint32_t h = fnv32_hash(pixels, len);
    h ^= fnv32_hash((unsigned char *)&config.saturation,    sizeof(config.saturation) );
    h ^= fnv32_hash((unsigned char *)&config.num_accents,   sizeof(config.num_accents));
    h ^= fnv32_hash((unsigned char *)&config.method,        sizeof(config.method));
    h ^= fnv32_hash((unsigned char *)&config.format,        sizeof(config.format));
    return h;
}

void get_cache_path(uint32_t hash, char *output, size_t size) 
{
    const char *cache_path = getenv("XDG_CACHE_HOME");
    char dir_path[PATH_MAX];

    if (cache_path) {
        snprintf(dir_path, sizeof(dir_path), "%s/pal", cache_path);
    } else {
        const char *home_path = getenv("HOME");
        if (home_path == NULL) {
            fprintf(stderr, "Error: no home env var found");
        }
        snprintf(dir_path, sizeof(dir_path), "%s/.cache/pal", home_path);
    }
    mkdir(dir_path, 0755);
    snprintf(output, size, "%s/%08X.bin", dir_path, hash);
}

char *args_shift(int *argc, char ***argv) {
    assert(*argc > 0);
    char *result = **argv;
    (*argc) -= 1;
    (*argv) += 1;
    return result;
}

int main(int argc, char **argv) {
    const char *program = args_shift(&argc, &argv);
    const char *template_file = NULL;
    Config config = { .bgfg = 1, .num_accents = 16, .saturation = 1.0, .method = 1, .format = HEX };

    if (argc <= 0) {
        printf("Usage: %s [arg1] [arg2] <image1> <image2> ...\n", program);
        printf("\n");
        printf("\t-n \tDont print background and foreground\n");
        printf("\t-c \tAmount of accent colors\n");
        printf("\t-s \tSaturation (float)\n");
        printf("\t-m \tColor picking method (0 - Area Average, 1 - K-Means)\n");
        printf("\t-f \tOutput format (0 - rgb, 1 - hex)\n");
        printf("\t-t \tTemplate to output\n");
        return 1;
    }

    while (argc > 0) {
        char *flag = args_shift(&argc, &argv);

        if (strcmp(flag,"-n") == 0) {
            config.bgfg = 0;
        }
        else if (strcmp(flag,"-c") == 0) {
            if (argc <= 0) {
                fprintf(stderr, "Error: %s requires a number\n", flag);
                return 1;
            }
            config.num_accents = atoi(args_shift(&argc, &argv));
        }
        else if (strcmp(flag,"-s") == 0) {
            if (argc <= 0) {
                fprintf(stderr, "Error: %s requires a float percentage\n", flag);
                return 1;
            }
            config.saturation = atof(args_shift(&argc, &argv));
        }
        else if (strcmp(flag,"-m") == 0) {
            if (argc <= 0) {
                fprintf(stderr, "Error: %s requires either 0 or 1\n", flag);
                return 1;
            }
            config.method = atoi(args_shift(&argc, &argv));
            if (config.method > 1 || config.method < 0) {
                return 1;
            }
        }
        else if (strcmp(flag,"-f") == 0) {
            if (argc <= 0) {
                fprintf(stderr, "Error: %s requires either 0 or 1\n", flag);
                return 1;
            }
            config.format = atoi(args_shift(&argc, &argv));
            if (config.format > 1 || config.format < 0) {
                return 1;
            }
        }
        else if (strcmp(flag,"-t") == 0) {
            if (argc <= 0) {
                fprintf(stderr, "Error: %s requires a file\n", flag);
                return 1;
            }
            template_file = args_shift(&argc, &argv);
        }
        else {
            const char *img_path = flag;
            int w, h, channels;
            uint8_t *pixels = stbi_load(img_path, &w, &h, &channels, 4);
            if (pixels == NULL ) {
                fprintf(stderr, "Error while loading image %s\n", img_path);
                return 1;
            }

            size_t total_bytes = (size_t)(w * h * channels);
            uint32_t hash = final_hash(pixels, total_bytes, config);

            char cache_path[PATH_MAX];
            get_cache_path(hash, cache_path, sizeof(cache_path));

            Color palette[16];
            Color bg, fg;
            FILE *f = fopen(cache_path, "rb");
            if (f) {
                fread(&bg, sizeof(Color), 1, f); 
                fread(&fg, sizeof(Color), 1, f);
                fread(palette, sizeof(Color), config.num_accents, f);
                fclose(f);
                stbi_image_free(pixels);
            } else {
                if (config.method == 0) {
                    generate_scheme(pixels, w, h, config, palette, &bg, &fg);
                } else {
                    generate_scheme_kmeans(pixels, w, h, config, palette, &bg, &fg);
                }
                FILE *fw = fopen(cache_path, "rb");
                if (fw) {
                    fwrite(&bg, sizeof(Color), 1, fw);
                    fwrite(&fg, sizeof(Color), 1, fw);
                    fwrite(palette, sizeof(Color), config.num_accents, fw);
                    fclose(fw);
                }
                stbi_image_free(pixels);
            }            
            
            if (template_file) {
                FILE *temp_f = fopen(template_file, "r");
                if (!temp_f) {
                    fprintf(stderr, "Error: could not read template file\n");
                }
                template_processor(temp_f, bg, fg, palette, config);
                fclose(temp_f);
            } else {
                switch (config.format) {
                    case 0:
                        if (config.bgfg) {
                            printf("rgb(%d, %d, %d)\n", bg.r, bg.g, bg.b);
                            printf("rgb(%d, %d, %d)\n", fg.r, fg.g, fg.b);
                        }
                        for (int i = 0; i < config.num_accents; i++) {
                            printf("rgb(%d, %d, %d)\n", palette[i].r, palette[i].g, palette[i].b);
                        }
                        break;
                    case 1:
                        if (config.bgfg) {
                            printf("#%02X%02X%02X\n", bg.r, bg.g, bg.b);
                            printf("#%02X%02X%02X\n", fg.r, fg.g, fg.b);
                        }
                        for (int i = 0; i < config.num_accents; i++) {
                            printf("#%02X%02X%02X\n", palette[i].r, palette[i].g, palette[i].b);
                        }
                        break;
                    }
            }
        }
    }
    return 0;
}