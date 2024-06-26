/*
    Render to BMP, using Signed Distance Function, instruction read from a file.

    Instruction typically look like ROUND(SEGMENT(POINT(0.61 0.01768) POINT(0.06 0.03713)) 0.00386)
    Each line of the file contain one instruction.
*/

#include "stdio.h"
#include "stdlib.h"
#include "sys/types.h"
#include "string.h"
#include "math.h"

// Return codes
#define OK 0
#define E_BOUND_REACHED -1
#define E_PARSE_UNSUPPORTED -10
#define E_PARSE_NUMBER -11
#define E_PARSE_ISEGMENT_BAD_INDEX -12
#define E_PARSE_NEED_LAYER -13
#define E_RENDER_INVALID_COORD -30

#define LOG_E(FORMAT, ...) fprintf(stderr, "%s:%d ERROR: " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__);
#define END_IF_NOK(X) if ((res = X) != OK) {return res;}
#define min(X, Y) ((X) < (Y) ? (X) : (Y))
#define max(X, Y) ((X) > (Y) ? (X) : (Y))
#define clamp(V, VMIN, VMAX) max(VMIN, min(VMAX, V))

#define CANVAS_WIDTH 800
#define CANVAS_HEIGHT 400
#define MAX_GEOMS_PER_LAYER 100
#define MAX_LAYER 100
static float DIAG;

// Geom types
#define POINT 0
#define SEGMENT 1

// Fusion types
#define F_MIN 0
#define F_SMIN 1

typedef struct Point {
    float x;
    float y;
    float rgba[4];
} Point;

typedef struct Segment {
    Point a;
    Point b;
} Segment;

typedef struct Geom {
    char type; // See "Geom types"
    Point point;
    Segment segment;
    float round_r;
} Geom;

typedef struct Layer {
    int fusion; // See "Fusion types"
    Geom geoms[MAX_GEOMS_PER_LAYER];
    size_t size;
} Layer;

typedef struct Scene {
    Layer layer[MAX_LAYER];
    size_t size;
} Scene;

typedef struct RichDistance {
    float d;
    float rgba[4];
} RichDistance;

// Definitions
int parse_line(Scene* scene, char* line, size_t* cursor, size_t line_size);
// ===

int parse_number(char* str, size_t* cursor, size_t stop, float* number) {
    char num[32];
    if (*cursor > stop) {
        LOG_E("Failed to read number, cursor value %ld is above stop %ld", *cursor, stop);
        return E_PARSE_NUMBER;
    }
    size_t start = *cursor;
    for (; *cursor < stop && (*cursor - start) < 32 && str[*cursor] != ' ' && str[*cursor] != ')'; *cursor += 1) {
        num[*cursor - start] = str[*cursor];
    }
    num[*cursor - start] = '\0';
    if (str[*cursor] != ' ' && str[*cursor] != ')') {
        LOG_E("Failed to read number, got: %s", num);
        return E_PARSE_NUMBER;
    }

    *number = atof(num);
    return OK;
}

int parse_int(char* str, size_t* cursor, size_t stop, int* number) {
    int res = OK;
    float x = 0;
    END_IF_NOK(parse_number(str, cursor, stop, &x))
    *number = (int) x;
    if (((float) *number) != x) {
        LOG_E("Failed to read number from: %s. Expected an integer.", str);
        res = E_PARSE_NUMBER;
    }

    return res;
}

// Parse COLOR(N N N N)
int parse_color(char* line, size_t* cursor, size_t line_size, Point* point) {
    int res = OK;
    *cursor += 6; // Skip COLOR(
    END_IF_NOK(parse_number(line, cursor, line_size, &(point->rgba[0])))
    *cursor += 1; // Skip the separator space
    END_IF_NOK(parse_number(line, cursor, line_size, &(point->rgba[1])))
    *cursor += 1; // Skip the separator space
    END_IF_NOK(parse_number(line, cursor, line_size, &(point->rgba[2])))
    *cursor += 1; // Skip the separator space
    END_IF_NOK(parse_number(line, cursor, line_size, &(point->rgba[3])))
    *cursor += 1; // Skip the )

    return res;
}

// Parse POINT(N N COLOR(...))
// COLOR(...) is optional
int parse_point(char* line, size_t* cursor, size_t line_size, Point* point) {
    int res = OK;
    *cursor += 6; // Skip POINT(
    END_IF_NOK(parse_number(line, cursor, line_size, &(point->x)))
    *cursor += 1; // Skip the separator space
    END_IF_NOK(parse_number(line, cursor, line_size, &(point->y)))
    *cursor += 1; // Skip the separator space
    if (line[*cursor] == 'C') {
        END_IF_NOK(parse_color(line, cursor, line_size, point))
    } else {
        point->rgba[0] = 1;
        point->rgba[1] = 0;
        point->rgba[2] = 1;
        point->rgba[3] = 1;
    }
    *cursor += 1; // Skip the )
    point->x *= CANVAS_WIDTH;
    point->y *= CANVAS_HEIGHT;

    return res;
}

// Parse SEGMENT(POINT(...) POINT(...))
int parse_segment(char* line, size_t* cursor, size_t line_size, Segment* seg) {
    int res = OK;
    *cursor += 8; // Skip SEGMENT(
    END_IF_NOK(parse_point(line, cursor, line_size, &(seg->a)))
    *cursor += 1; // Skip the separator space
    END_IF_NOK(parse_point(line, cursor, line_size, &(seg->b)))
    *cursor += 1; // Skip the )

    return res;
}

// Parse ISEGMENT(N N)
// Where N is the index of a Point Geom in the Layer
int parse_isegment(Layer* layer, char* line, size_t* cursor, size_t line_size, Segment* seg) {
    int res = OK;
    int ia, ib;
    *cursor += 9; // Skip ISEGMENT(
    END_IF_NOK(parse_int(line, cursor, line_size, &ia))
    *cursor += 1; // Skip the separator space
    END_IF_NOK(parse_int(line, cursor, line_size, &ib))
    *cursor += 1; // Skip the )

    if (ia >= 0 && ia < layer->size && layer->geoms[ia].type == POINT) {
        seg->a = layer->geoms[ia].point;
    } else {
        LOG_E("Bad Point Geom index %d", ia);
        return E_PARSE_ISEGMENT_BAD_INDEX;
    }
    if (ib >= 0 && ib < layer->size && layer->geoms[ib].type == POINT) {
        seg->b = layer->geoms[ib].point;
    } else {
        LOG_E("Bad Point Geom index %d", ib);
        return E_PARSE_ISEGMENT_BAD_INDEX;
    }

    return res;
}

// Parse ROUND(N ...)
int parse_round(Scene* scene, char* line, size_t* cursor, size_t line_size, Geom* geom) {
    int res = OK;
    *cursor += 6; // Skip ROUND(
    END_IF_NOK(parse_number(line, cursor, line_size, &(geom->round_r)))
    *cursor += 1; // Skip the separator space
    END_IF_NOK(parse_line(scene, line, cursor, line_size))
    *cursor += 1; // Skip the )
    geom->round_r *= DIAG;

    return res;
}

// Parse LAYER(N)
// Where N can be any of "Fusion types"
int parse_layer(Scene* scene, char* line, size_t* cursor, size_t line_size) {
    int res = OK;
    int fusion;
    *cursor += 6; // Skip LAYER(
    END_IF_NOK(parse_int(line, cursor, line_size, &fusion))
    *cursor += 1; // Skip the )

    scene->size += 1;
    scene->layer[scene->size - 1].fusion = fusion;
    scene->layer[scene->size - 1].size = 0;

    return res;
}

int parse_line(Scene* scene, char* line, size_t* cursor, size_t line_size) {
    int res = 0;
    char wkt_type[32];
    size_t wkt_type_size = 0;
    for (wkt_type_size = 0; wkt_type_size < 32 && (*cursor + wkt_type_size) < line_size && line[*cursor + wkt_type_size] != '('; wkt_type_size++) {
        wkt_type[wkt_type_size] = line[*cursor + wkt_type_size];
    }
    wkt_type[wkt_type_size] = '\0';

    if (strcmp(wkt_type, "LAYER") == 0) {
        if (scene->size >= MAX_LAYER) {
            LOG_E("Reached max layer count %d", MAX_LAYER);
            return E_BOUND_REACHED;
        }
        END_IF_NOK(parse_layer(scene, line, cursor, line_size))
        return OK;
    }

    if (scene->size == 0) {
        LOG_E("Trying to create geometries without layer, your first instruction should be a LAYER, got %s", line);
        return E_PARSE_NEED_LAYER;
    }

    Layer* layer = &(scene->layer[scene->size-1]);
    if (layer->size >= MAX_GEOMS_PER_LAYER) {
        LOG_E("Reached max geom count %d for layer %ld", MAX_GEOMS_PER_LAYER, scene->size-1);
        return E_BOUND_REACHED;
    }
    if (strcmp(wkt_type, "ROUND") == 0) {
        END_IF_NOK(parse_round(scene, line, cursor, line_size, &(layer->geoms[layer->size])))
    } else if (strcmp(wkt_type, "POINT") == 0) {
        layer->geoms[layer->size].type = POINT;
        END_IF_NOK(parse_point(line, cursor, line_size, &(layer->geoms[layer->size].point)))
        layer->size += 1;
    } else if (strcmp(wkt_type, "SEGMENT") == 0) {
        layer->geoms[layer->size].type = SEGMENT;
        END_IF_NOK(parse_segment(line, cursor, line_size, &(layer->geoms[layer->size].segment)))
        layer->size += 1;
    } else if (strcmp(wkt_type, "ISEGMENT") == 0) {
        layer->geoms[layer->size].type = SEGMENT;
        END_IF_NOK(parse_isegment(layer, line, cursor, line_size, &(layer->geoms[layer->size].segment)))
        layer->size += 1;
    } else {
        LOG_E("Unsuported word %s in line %s", wkt_type, line);
        return E_PARSE_UNSUPPORTED;
    }

    return res;
}

/* Signed Distance Functions, from https://iquilezles.org/articles/distfunctions2d/ */

float length(Point p) {
    return sqrt(p.x*p.x + p.y*p.y);
}

float mix(float x, float y, float a) {
    return x*(1-a) + y*a;
}

Point sub(Point a, Point b) {
    Point c = {a.x - b.x, a.y - b.y};
    return c;
}

Point mul(Point a, float s) {
    Point c = {a.x * s, a.y * s};
    return c;
}

float dot(Point a, Point b) {
    return a.x*b.x + a.y*b.y;
}

// Smooth min using the root method
float smin( float a, float b, float k ) {
    k *= 2.0;
    float x = b-a;
    return 0.5*(a+b-sqrt(x*x+k*k));
}

// Smooth min using the quadratic method
Point sminq( float a, float b, float k )
{
    float h = 1.0 - min( abs(a-b)/(6.0*k), 1.0 );
    float w = h*h*h;
    float m = w*0.5;
    float s = w*k;
    Point ra = {a-s,m};
    Point rb = {b-s,1.0-m};
    return (a<b) ? ra : rb;
}

RichDistance sdMin(RichDistance a, RichDistance b) {
    Point sd = sminq(a.d, b.d, 1);
    RichDistance rd;
    rd.d = sd.x;
    rd.rgba[0] = mix(a.rgba[0], b.rgba[0], sd.y);
    rd.rgba[1] = mix(a.rgba[1], b.rgba[1], sd.y);
    rd.rgba[2] = mix(a.rgba[2], b.rgba[2], sd.y);
    rd.rgba[3] = mix(a.rgba[3], b.rgba[3], sd.y);
    return rd;
}

RichDistance opRound(RichDistance rd, float r) {
    rd.d -= r; 
    return rd;
}

RichDistance sdPoint(Point p, Point a) {
    RichDistance rd;
    rd.d = length(sub(p, a));
    rd.rgba[0] = a.rgba[0];
    rd.rgba[1] = a.rgba[1];
    rd.rgba[2] = a.rgba[2];
    rd.rgba[3] = a.rgba[3];
    return rd;
}

RichDistance sdSegment(Point p, Point a, Point b ) {
    RichDistance rd;
    Point pa = sub(p, a);
    Point ba = sub(b, a);
    float h = clamp(dot(pa,ba)/dot(ba,ba), 0.0, 1.0);
    rd.d = length(sub(pa, mul(ba, h)));
    rd.rgba[0] = a.rgba[0]*(1-h) + b.rgba[0]*h;
    rd.rgba[1] = a.rgba[1]*(1-h) + b.rgba[1]*h;
    rd.rgba[2] = a.rgba[2]*(1-h) + b.rgba[2]*h;
    rd.rgba[3] = a.rgba[3]*(1-h) + b.rgba[3]*h;
    return rd;
}

void sdRenderLayer(Layer layer, float x, float y, float pixel[4]) {
    RichDistance d = {CANVAS_HEIGHT*CANVAS_WIDTH, {0, 0, 0, 0}};
    Point p = {x, y};
    for (size_t i = 0; i < layer.size; i++) {
        RichDistance gd;
        switch (layer.geoms[i].type)
        {
        case POINT:
            gd = opRound(sdPoint(p, layer.geoms[i].point), layer.geoms[i].round_r);
            break;
        case SEGMENT:
            gd = opRound(sdSegment(p, layer.geoms[i].segment.a, layer.geoms[i].segment.b), layer.geoms[i].round_r);
            break;
        default:
            break;
        }

        switch (layer.fusion)
        {
        case F_MIN:
            d = sdMin(d, gd);
            break;
        case F_SMIN:
            d = sdMin(d, gd);
            break;
        default:
            break;
        }
    }
    float opacity = clamp(0.5-d.d, 0, 1);
    pixel[0] = d.rgba[0];
    pixel[1] = d.rgba[1];
    pixel[2] = d.rgba[2];
    pixel[3] = d.rgba[3]*opacity;
}

void sdRenderScene(Scene* scene, float x, float y, unsigned char pixel[3]) {
    for (int i = 0; i < scene->size; i++) {
        float pixel4[4] = {0, 0, 0, 0};
        sdRenderLayer(scene->layer[i], x, y, pixel4);
        if (pixel4[3] > 0) {
            pixel[0] = (unsigned char) (pixel4[2] * 255 * pixel4[3]);
            pixel[1] = (unsigned char) (pixel4[1] * 255 * pixel4[3]);
            pixel[2] = (unsigned char) (pixel4[0] * 255 * pixel4[3]);
        }
    }
}

/* === */


/* BMP metadata, taken from https://stackoverflow.com/a/47785639 CC BY-SA 4.0 */

const int BYTES_PER_PIXEL = 3;
const int FILE_HEADER_SIZE = 14;
const int INFO_HEADER_SIZE = 40;

unsigned char* create_bitmap_file_header(int height, int stride) {
    int fileSize = FILE_HEADER_SIZE + INFO_HEADER_SIZE + (stride * height);

    static unsigned char fileHeader[] = {
        0,0,     /// signature
        0,0,0,0, /// image file size in bytes
        0,0,0,0, /// reserved
        0,0,0,0, /// start of pixel array
    };

    fileHeader[ 0] = (unsigned char)('B');
    fileHeader[ 1] = (unsigned char)('M');
    fileHeader[ 2] = (unsigned char)(fileSize      );
    fileHeader[ 3] = (unsigned char)(fileSize >>  8);
    fileHeader[ 4] = (unsigned char)(fileSize >> 16);
    fileHeader[ 5] = (unsigned char)(fileSize >> 24);
    fileHeader[10] = (unsigned char)(FILE_HEADER_SIZE + INFO_HEADER_SIZE);

    return fileHeader;
}

unsigned char* create_bitmap_info_header(int height, int width) {
    static unsigned char infoHeader[] = {
        0,0,0,0, /// header size
        0,0,0,0, /// image width
        0,0,0,0, /// image height
        0,0,     /// number of color planes
        0,0,     /// bits per pixel
        0,0,0,0, /// compression
        0,0,0,0, /// image size
        0,0,0,0, /// horizontal resolution
        0,0,0,0, /// vertical resolution
        0,0,0,0, /// colors in color table
        0,0,0,0, /// important color count
    };

    infoHeader[ 0] = (unsigned char)(INFO_HEADER_SIZE);
    infoHeader[ 4] = (unsigned char)(width      );
    infoHeader[ 5] = (unsigned char)(width >>  8);
    infoHeader[ 6] = (unsigned char)(width >> 16);
    infoHeader[ 7] = (unsigned char)(width >> 24);
    infoHeader[ 8] = (unsigned char)(height      );
    infoHeader[ 9] = (unsigned char)(height >>  8);
    infoHeader[10] = (unsigned char)(height >> 16);
    infoHeader[11] = (unsigned char)(height >> 24);
    infoHeader[12] = (unsigned char)(1);
    infoHeader[14] = (unsigned char)(BYTES_PER_PIXEL*8);

    return infoHeader;
}

/* === */

int write_bitmap(Scene* scene, char* imageFileName)
{
    int widthInBytes = CANVAS_WIDTH * BYTES_PER_PIXEL;

    unsigned char padding[3] = {0, 0, 0};
    int paddingSize = (4 - (widthInBytes) % 4) % 4;

    int stride = (widthInBytes) + paddingSize;

    FILE* imageFile = fopen(imageFileName, "wb");

    unsigned char* fileHeader = create_bitmap_file_header(CANVAS_HEIGHT, stride);
    fwrite(fileHeader, 1, FILE_HEADER_SIZE, imageFile);

    unsigned char* infoHeader = create_bitmap_info_header(CANVAS_HEIGHT, CANVAS_WIDTH);
    fwrite(infoHeader, 1, INFO_HEADER_SIZE, imageFile);

    for (int y = 0; y < CANVAS_HEIGHT; y++) {
        for (int x = 0; x < CANVAS_WIDTH; x++) {
            unsigned char pixel[] = {0, 0, 0};
            sdRenderScene(scene, x, y, pixel);
            fwrite(pixel, BYTES_PER_PIXEL, 1, imageFile);
        }
        fwrite(padding, 1, paddingSize, imageFile);
    }

    fclose(imageFile);
}

int main(void) {
    DIAG = sqrt(CANVAS_WIDTH*CANVAS_WIDTH + CANVAS_HEIGHT*CANVAS_HEIGHT);
    Scene scene;
    scene.size = 0;

    FILE* fp;
    char* line = NULL;
    size_t len = 0;
    ssize_t read;

    fp = fopen("data.wkt", "r");
    if (fp == NULL)
        exit(EXIT_FAILURE);

    int res = OK;
    while ((read = getline(&line, &len, fp)) != -1) {
        size_t cursor = 0;
        line[--read] = '\0'; // Remove LF (will break on LFLR)
        if((res = parse_line(&scene, line, &cursor, read)) == OK) {
        } else {
            LOG_E("Got error %d for line: %s", res, line);
        }
    }

    write_bitmap(&scene, "canvas.bmp");

    fclose(fp);
    if (line)
        free(line);
    exit(EXIT_SUCCESS);
}
