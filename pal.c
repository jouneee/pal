#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <dirent.h>
#include <limits.h>
#include <errno.h>

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define MAX_PROVIDED_IMAGES 128
#define MAX_TEMPLATES 128
#define MAX_PLACEHOLDER_LEN 32
#define SAMPLE_COUNT 1024
#define PALETTE_COUNT 16

#define MAX(a, b) ((a > b) ? a : b)
#define MIN(a, b) ((a < b) ? a : b)
#define CLAMP(a, b, c) (a < b) ? b : (a > c) ? c : a

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
    int silent;
    int using_template;
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
    int max = MAX(c.r, MAX(c.g, c.b));
    int min = MIN(c.r, MIN(c.g, c.b));
    return max - min;
}

int compare_vibrancy(const void *a, const void *b) {
    return ((Color*)b)->vibrancy - ((Color*)a)->vibrancy;
}

void apply_saturation(Color *c, float saturation) {
    if (saturation == 1.0) return;
    float gray = c->luminance * 255.0f;

    c->r = (uint8_t)(CLAMP((gray + saturation * (c->r - gray)), 0, 255));
    c->g = (uint8_t)(CLAMP((gray + saturation * (c->g - gray)), 0, 255));
    c->b = (uint8_t)(CLAMP((gray + saturation * (c->b - gray)), 0, 255));
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
    Color selected[PALETTE_COUNT];
    for (int i = 0; i < count && picked < PALETTE_COUNT; i++) {
        int distinct = 1;
        
        // dont pick colors too close to bg or fg
        float diff_bg = fabsf(samples[i].luminance - darkest.luminance);
        float diff_fg = fabsf(samples[i].luminance - lightest.luminance);
        if (diff_bg < 0.08f || diff_fg < 0.08f) distinct = 0;

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
        for (int k = 0; k < PALETTE_COUNT; k++) {    
            if (counts[k] > 0) {
                centers[k].r = (uint8_t)(r_sum[k] / counts[k]);
                centers[k].g = (uint8_t)(g_sum[k] / counts[k]);
                centers[k].b = (uint8_t)(b_sum[k] / counts[k]);
                centers[k].luminance = get_luminance(centers[k]);
                centers[k].vibrancy = calculate_vibrancy(centers[k]);
            }
        }
    }
    for (int k = 0; k < PALETTE_COUNT; k++) {
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
    char *result = NULL;
    if (fseek(template_file, 0L, SEEK_END) == 0) {
        long bufsize = ftell(template_file);
        if(bufsize == -1) {
            fprintf(stderr, "Error: template file is corrupted\n");
            return NULL;
        }
        
        result = malloc(sizeof(char) * (bufsize + 1));
        if (fseek(template_file, 0L, SEEK_SET) != 0) {
            free(result);
            return NULL;
        }
        char bg_str[20], fg_str[20];
        color_to_string(bg, config.format, bg_str, sizeof(bg_str));
        color_to_string(fg, config.format, fg_str, sizeof(fg_str));
        
        char **accent_strings = malloc(PALETTE_COUNT * sizeof(char*));
        for (int i = 0; i < PALETTE_COUNT; i++) {
            accent_strings[i] = malloc(20);
            color_to_string(palette[i], config.format, accent_strings[i], 20);
        }
        
        enum { OUTSIDE, INSIDE } state = OUTSIDE;
        char placeholder[MAX_PLACEHOLDER_LEN];
        int len = 0;
        int p_pos = 0;
        int c;
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
                            if (i >= 0 && i < PALETTE_COUNT) target = accent_strings[i];
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
    return NULL;
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
    h ^= fnv32_hash((unsigned char *)&config.method,            sizeof(config.method));
    h ^= fnv32_hash((unsigned char *)&config.format,            sizeof(config.format));
    h ^= fnv32_hash((unsigned char *)&config.using_template,    sizeof(config.using_template));
    h ^= fnv32_hash((unsigned char *)&st.st_mtim,               sizeof(st.st_mtim));
    h ^= fnv32_hash((unsigned char *)&st.st_size,               sizeof(st.st_size));
    return h;
}

char *args_shift(int *argc, char ***argv) {
    assert(*argc > 0);
    char *result = **argv;
    (*argc) -= 1;
    (*argv) += 1;
    return result;
}

void directory_helper(const char *home) {
    if (!home) fprintf(stderr, "HOME env not set\n"), 1;
    char path[PATH_MAX];
    snprintf(path, sizeof(path), "%s/.cache/pal", home);
    mkdir(path, 0755);

    snprintf(path, sizeof(path), "%s/.cache/pal/other", home);
    mkdir(path, 0755);
}

void get_templates_path(const char *home, char *out, size_t size) {
    snprintf(out, size, "%s/.config/pal/", home);
}

void get_templates(char* templ_dir, char (*out)[MAX_TEMPLATES], int *count) {
    DIR *d = opendir(templ_dir);
    if (!d) {
        *count = 0; 
        return;
    } 
    struct dirent *e;
    *count = 0;
    while ((e = readdir(d)) && *count < MAX_PROVIDED_IMAGES) {
        if (e->d_type != DT_REG) continue;
        strncpy(out[*count], e->d_name, 255);
        out[*count][255] = '\0';
        (*count)++;
    }
    closedir(d);
}

void get_palettes_cache_path(uint32_t hash, const char *home, char *out, size_t size) {
    snprintf(out, size, "%s/.cache/pal/other/%08X", home, hash);
}

void get_templates_cache_path(const char *home, const char *templ, char *out, size_t size) {
    snprintf(out, size, "%s/.cache/pal/%s", home, templ);
}

int main(int argc, char **argv) {
    const char *program = args_shift(&argc, &argv);
    Config config = {   .bgfg = 1,
                        .silent = 0,
                        .saturation = 1.0, 
                        .method = KMEANS, 
                        .format = HEX };

    const char *input_files[MAX_PROVIDED_IMAGES];
    int input_count = 0;

    if (argc <= 0) {
        printf("Usage: %s [arg1] [arg2] <image1> <image2> ...\n", program);
        printf("\n");
        printf("\t-n \tDont print background and foreground\n");
        printf("\t-nv\tNo output\n");
        printf("\t-s \tSaturation (float)\n");
        printf("\t-m \tColor picking method (0 - Area Average, 1 - K-Means)\n");
        printf("\t-f \tOutput format (0 - rgb, 1 - hex)\n");
        return 1;
    }

    while (argc > 0) {
        char *arg = args_shift(&argc, &argv);
        if (strcmp(arg,"-n") == 0) {
            config.bgfg = 0;
        }
        else if (strcmp(arg, "-nv") == 0) {
            config.silent = 1;
        }
        else if (strcmp(arg, "-s") == 0) {
            if (argc <= 0) return fprintf(stderr, "Error: %s requires a float percentage\n", arg), 1;

            config.saturation = atof(args_shift(&argc, &argv));
            if (config.saturation > 5.0 || config.saturation < 0.0) return 1;
        }
        else if (strcmp(arg,"-m") == 0) {
            if (argc <= 0) return fprintf(stderr, "Error: %s requires either 0 or 1\n", arg), 1;
            
            config.method = atoi(args_shift(&argc, &argv));
            if (config.method > 1 || config.method < 0) return 1;
        }
        else if (strcmp(arg,"-f") == 0) {
            if (argc <= 0) fprintf(stderr, "Error: %s requires either 0 or 1\n", arg), 1;

            config.format = atoi(args_shift(&argc, &argv));
            if (config.format > 1 || config.format < 0) return 1;
        }
        else {
            if (input_count < MAX_PROVIDED_IMAGES) {
                input_files[input_count++] = arg;
            } else {
                fprintf(stderr, "Error: too many inputs, skipping ...");
            }
        }
    }

    const char *home = getenv("HOME");
    if (!home) fprintf(stderr, "HOME not set\n"), 1;
    directory_helper(home);

    char template_dir[PATH_MAX]; // ~/.config/pal/
    get_templates_path(home, template_dir, sizeof(template_dir));

    char template_files[MAX_PROVIDED_IMAGES][MAX_TEMPLATES];
    int template_count = 0;
    get_templates(template_dir, template_files, &template_count);

    for (int i = 0; i < input_count; i++) {
        const char *img_path = input_files[i];
        uint32_t hash = hash_metadata(img_path, config);
        char palettes_cache_path[PATH_MAX]; // ~/.cache/other/pal/
        get_palettes_cache_path(hash, home, palettes_cache_path, sizeof(palettes_cache_path));

        Color palette[16];
        Color bg, fg;
        int cached_palette = 0;
        FILE *f = fopen(palettes_cache_path, "r");
        if (f) {
            fread(&bg,     sizeof(Color), 1, f); 
            fread(&fg,     sizeof(Color), 1, f);
            fread(palette, sizeof(Color), PALETTE_COUNT, f);
            cached_palette = 1;
        }
        if (!cached_palette) {
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
            FILE *fw = fopen(palettes_cache_path, "w");
            if (fw) {
                fwrite(&bg, sizeof(Color), 1, fw);
                fwrite(&fg, sizeof(Color), 1, fw);
                fwrite(palette, sizeof(Color), PALETTE_COUNT, fw);
                fclose(fw);
            }
            stbi_image_free(pixels);
        }
        for (int t = 0; t < template_count; t++) {
            char src[PATH_MAX];
            snprintf(src, sizeof(src), "%s/%s", template_dir, template_files[t]);
            
            FILE *templ = fopen(src, "r");
            if (!templ) continue;
            
            char *result = template_processor(templ, bg, fg, palette, config);
            fclose(templ);
            if (!result) continue;
            
            char out[PATH_MAX];
            get_templates_cache_path(home, template_files[t], out, sizeof(out)); // ~/.cache/pal/
            
            FILE *output = fopen(out, "w");
            if (output) {
                fputs(result, output);
                fclose(output);
            }
            free(result);
        }
        if (!config.silent) {
            const char *pstring = (config.format == 1) ? "#%02X%02X%02X\n" : "rgb(%d, %d, %d)\n";
            if (config.bgfg) {
                printf(pstring, bg.r, bg.g, bg.b);
                printf(pstring, fg.r, fg.g, fg.b);
            }
            for (int i = 0; i < PALETTE_COUNT; i++) {
                printf(pstring, palette[i].r, palette[i].g, palette[i].b);
            }
        }
    }
    return 0;
}