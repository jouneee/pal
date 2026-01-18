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
    int using_template;
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

void apply_saturation(Color *c, float saturation) {
    if (saturation == 1.0) return;
    float gray = c->luminance * 255.0f;

    c->r = clamp_u8(gray + saturation * (c->r - gray));
    c->g = clamp_u8(gray + saturation * (c->g - gray));
    c->b = clamp_u8(gray + saturation * (c->b - gray));
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
        apply_saturation(&selected[k], config.saturation);
        out_palette[k] = selected[k];
    }
    apply_saturation(&darkest, config.saturation);
    apply_saturation(&lightest, config.saturation);
    *out_bg = darkest;
    *out_fg = lightest;
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
        apply_saturation(&centers[k], config.saturation);
        out_palette[k] = centers[k];
    }
    apply_saturation(&darkest, config.saturation);
    apply_saturation(&lightest, config.saturation);
    *out_bg = darkest;
    *out_fg = lightest;
}

static void color_to_string(Color c, Format format, char *buf, size_t buflen) {
    if (format == RGB) {
        snprintf(buf, buflen, "rgb(%d, %d, %d)", c.r, c.g, c.b);
    } else {
        snprintf(buf, buflen, "#%02X%02X%02X", c.r, c.g, c.b);
    }
}

char *template_processor(FILE *template_file, Color bg, Color fg, Color *palette, Config config) {
    struct stat st;
    if (fstat(fileno(template_file), &st) != 0) {
        return 0;
    }
    long filesize = st.st_size;
    size_t capacity = filesize + 1024;
    char *result = malloc(capacity);
    if (!result) return NULL;

    size_t len = 0;

    char bg_str[20], fg_str[20];
    color_to_string(bg, config.format, bg_str, sizeof(bg_str));
    color_to_string(fg, config.format, fg_str, sizeof(fg_str));

    char **accent_strings = malloc(config.num_accents * sizeof(char*));
    for (int i = 0; i < config.num_accents; i++) {
        accent_strings[i] = malloc(20);
        color_to_string(palette[i], config.format, accent_strings[i], 20);
    }
    
    enum { OUTSIDE, INSIDE } state = OUTSIDE;
    char placeholder[MAX_PLACEHOLDER_LEN];
    int p_pos = 0;
    int c;

    rewind(template_file);
    while ((c = fgetc(template_file)) != EOF) {
        switch (state) {
            case OUTSIDE:
                if (c == '`') {
                    state = INSIDE;
                    p_pos = 0;
                } else {
                    result[len++] = (char)c;
                }
                break;
            case INSIDE:
                if (c == '`') {
                    placeholder[p_pos] = '\0';
                    char *target = NULL;

                    if (strcmp(placeholder, "@background") == 0) target = bg_str;
                    else if (strcmp(placeholder, "@foreground") == 0) target = fg_str;
                    else if (strncmp(placeholder, "@color", 6) == 0) {
                        int i = atoi(placeholder + 6);
                        if (i >= 0 && i < config.num_accents) target = accent_strings[i];
                    }
                    if (target) {
                        size_t t_len = strlen(target);
                        memcpy(result + len, target, t_len);
                        len += t_len;
                    } else {
                        result[len++] = '`';
                        size_t p_len = strlen(placeholder);
                        memcpy(result + len, placeholder, p_len);
                        len += p_len;
                        result[len++] = '`';
                    }
                    state = OUTSIDE;
                } else if (p_pos < MAX_PLACEHOLDER_LEN - 1) {
                    placeholder[p_pos++] = (char)c;
                }
            break;
        }
    }
    result[len] = '\0';
    free(accent_strings);
    return result;
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

uint32_t hash_metadata(const char *filename, Config config) 
{
    struct stat st;
    if (stat(filename, &st) != 0) {
        return 0;
    }
    
    uint32_t h = fnv32_hash((unsigned char *)filename,          strlen(filename));
    h ^= fnv32_hash((unsigned char *)&config.saturation,        sizeof(config.saturation) );
    h ^= fnv32_hash((unsigned char *)&config.num_accents,       sizeof(config.num_accents));
    h ^= fnv32_hash((unsigned char *)&config.method,            sizeof(config.method));
    h ^= fnv32_hash((unsigned char *)&config.format,            sizeof(config.format));
    h ^= fnv32_hash((unsigned char *)&config.using_template,    sizeof(config.using_template));
    h ^= fnv32_hash((unsigned char *)&st.st_mtim,               sizeof(st.st_mtim));
    h ^= fnv32_hash((unsigned char *)&st.st_size,               sizeof(st.st_size));
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
    Config config = {   .bgfg = 1, 
                        .num_accents = 16, 
                        .saturation = 1.0, 
                        .method = KMEANS, 
                        .format = HEX, 
                        .using_template = 1 };

    const char *input_files[128];
    int input_count = 0;

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
        char *arg = args_shift(&argc, &argv);
        if (strcmp(arg,"-n") == 0) {
            config.bgfg = 0;
        }
        else if (strcmp(arg,"-c") == 0) {
            if (argc <= 0) {
                return fprintf(stderr, "Error: %s requires a number\n", arg), 1;
            }
            config.num_accents = atoi(args_shift(&argc, &argv));
        }
        else if (strcmp(arg,"-s") == 0) {
            if (argc <= 0) {
                return fprintf(stderr, "Error: %s requires a float percentage\n", arg), 1;
            }
            config.saturation = atof(args_shift(&argc, &argv));
        }
        else if (strcmp(arg,"-m") == 0) {
            if (argc <= 0) {
                return fprintf(stderr, "Error: %s requires either 0 or 1\n", arg), 1;
            }
            config.method = atoi(args_shift(&argc, &argv));
            if (config.method > 1 || config.method < 0) {
                return 1;
            }
        }
        else if (strcmp(arg,"-f") == 0) {
            if (argc <= 0) {
                fprintf(stderr, "Error: %s requires either 0 or 1\n", arg);
                return 1;
            }
            config.format = atoi(args_shift(&argc, &argv));
            if (config.format > 1 || config.format < 0) {
                return 1;
            }
        }
        else if (strcmp(arg,"-t") == 0) {
            if (argc <= 0) {
                fprintf(stderr, "Error: %s requires a file\n", arg);
                return 1;
            }
            template_file = args_shift(&argc, &argv);
            config.using_template = strlen(template_file);
        }
        else {
            if (input_count < 128) {
                input_files[input_count++] = arg;
            } else {
                fprintf(stderr, "Error: too many inputs, skipping ...");
            }
        }
    }
    for (int i = 0; i < input_count; i++) {
        const char *img_path = input_files[i];
        uint32_t hash = hash_metadata(img_path, config);
        char cache_path[PATH_MAX];
        get_cache_path(hash, cache_path, sizeof(cache_path));
    
        Color palette[16];
        Color bg, fg;
        int cached = 0;
        FILE *f = fopen(cache_path, "rb");
        if (f) {
            fread(&bg, sizeof(Color), 1, f); 
            fread(&fg, sizeof(Color), 1, f);
            fread(palette, sizeof(Color), config.num_accents, f);
            cached = 1;
        }
        if (!cached) {
            int w, h, channels;
            uint8_t *pixels = stbi_load(img_path, &w, &h, &channels, 4);
            if (pixels == NULL ) {
                fprintf(stderr, "Error while loading image %s\n", img_path);
                return 1;
            }

            if (config.method == 0) {
                generate_scheme(pixels, w, h, config, palette, &bg, &fg);
            } else {
                generate_scheme_kmeans(pixels, w, h, config, palette, &bg, &fg);
            }
            FILE *fw = fopen(cache_path, "wb");
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
            char *result = template_processor(temp_f, bg, fg, palette, config);
            fclose(temp_f);
            if (result) {
                fputs(result, stdout);
                FILE *fw = fopen(cache_path, "wb");
                if (fw) {
                    fwrite(result, 1, strlen(result), fw);
                    fclose(fw);
                }
                free(result);
            }
        } else {
            const char *pstring = (config.format == 1) ? "#%02X%02X%02X\n" : "rgb(%d, %d, %d)\n";

            if (config.bgfg) {
                printf(pstring, bg.r, bg.g, bg.b);
                printf(pstring, fg.r, fg.g, fg.b);
            }
            for (int i = 0; i < config.num_accents; i++) {
                printf(pstring, palette[i].r, palette[i].g, palette[i].b);
            }
        }
    }
    return 0;
}