/*
    Author: Hugo Roussel, unless indicated otherwise.
    License: MIT, with parts in CC BY-SA 4.0.

    Render to BMP, using Signed Distance Function, instruction read from a file.

    Instruction typically look like:
        LAYER(1)
        POINT(0.61 0.01768)
        POINT(0.06 0.03713) 
        ROUND(0.01 SEGMENT(0 1))
        POINT(-0.5 0.8)
        ROUND(0.01 BEZIER(0 1 3))
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
#define E_FILE_OPEN -40

#define LOG_E(FORMAT, ...) fprintf(stderr, "%s:%d ERROR: " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__);
#define END_IF_NOK(X) if ((res = X) != OK) {return res;}

/* Math macro, inspired by GLSL function */
// Min
#define min(X, Y) ((X) < (Y) ? (X) : (Y))
// Max
#define max(X, Y) ((X) > (Y) ? (X) : (Y))
// Clamp value V between VMIN and VMAX
#define clamp(V, VMIN, VMAX) max(VMIN, min(VMAX, V))
// Get the value at A (0 to 1) for linear interpolation from X to Y
#define mix(X, Y, A) (X*(1-A) + Y*A)
// Mix, for an array of size 4. R is the result array, X and Y are being mixed, A is the scalar position of the interpolation
#define mix4(R, X, Y, A) R[0] = mix(X[0], Y[0], A); R[1] = mix(X[1], Y[1], A); R[2] = mix(X[2], Y[2], A); R[3] = mix(X[3], Y[3], A);
// Copy an array[4] B into A
#define copy4(A, B) A[0] = B[0]; A[1] = B[1]; A[2] = B[2]; A[3] = B[3];

#define CANVAS_WIDTH 800
#define CANVAS_HEIGHT 800
#define MAX_GEOMS_PER_LAYER 500
#define MAX_LAYER 5
#define BEZIER_LUT_SIZE 31
#define MAX_BEZIER_POINT 11
// Binomial coeficient for n=MAX_BEZIER_POINT-1, because coeficient are calculated on [0,n] but C index is [0,n[ 
const float binom_coef[MAX_BEZIER_POINT][MAX_BEZIER_POINT] = {
    {1, 0,  0,  0,   0,   0,   0,   0,   0,  0,  0},
    {1, 1,  0,  0,   0,   0,   0,   0,   0,  0,  0},
    {1, 2,  1,  0,   0,   0,   0,   0,   0,  0,  0},
    {1, 3,  3,  1,   0,   0,   0,   0,   0,  0,  0},
    {1, 4,  6,  4,   1,   0,   0,   0,   0,  0,  0},
    {1, 5,  10, 10,  5,   1,   0,   0,   0,  0,  0},
    {1, 6,  15, 20,  15,  6,   1,   0,   0,  0,  0},
    {1, 7,  21, 35,  35,  21,  7,   1,   0,  0,  0},
    {1, 8,  28, 56,  70,  56,  28,  8,   1,  0,  0},
    {1, 9,  36, 84,  126, 126, 84,  36,  9,  1,  0},
    {1, 10, 45, 120, 210, 252, 210, 120, 45, 10, 1}
};
static float DIAG;

// Geom types
#define POINT 0
#define SEGMENT 1
#define BEZIER 2

// Fusion types
#define F_MIN 0
#define F_SMIN 1

typedef struct Geom Geom;
typedef struct Segment Segment;
typedef struct Bezier Bezier;
typedef struct Point Point;
typedef struct Layer Layer;
typedef struct Scene Scene;
typedef struct RichDistance RichDistance; 

typedef struct Vec2 {
    float x;
    float y;
} Vec2;

struct Point {
    Vec2 v;
    float rgba[4];
};

struct Segment {
    Geom* a;
    Geom* b;
};

struct Bezier {
    Geom* points[MAX_BEZIER_POINT];
    size_t size;
    Vec2 lut[BEZIER_LUT_SIZE]; // TODO: this is expensive as the lut exist even for non bezier geom
};

struct Geom {
    char type; // See "Geom types"
    Point point;
    Segment segment;
    Bezier bezier;
    float round_r;
};

struct Layer {
    int fusion; // See "Fusion types"
    Geom geoms[MAX_GEOMS_PER_LAYER];
    size_t size;
};

struct Scene {
    Layer layer[MAX_LAYER];
    size_t size;
};

struct RichDistance {
    float d;
    float rgba[4];
};

// function Definitions
int parse_line(Scene* scene, char* line, size_t* cursor, size_t line_size);
// Vec2 quadraticBezier(float t, Vec2 A, Vec2 B, Vec2 C);
Vec2 bezier(float t, Bezier* B);
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
    END_IF_NOK(parse_number(line, cursor, line_size, &(point->v.x)))
    *cursor += 1; // Skip the separator space
    END_IF_NOK(parse_number(line, cursor, line_size, &(point->v.y)))
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
    point->v.x *= CANVAS_WIDTH;
    point->v.y *= CANVAS_HEIGHT;

    return res;
}

// Parse SEGMENT(N N)
// Where N is the index of a Point Geom in the Layer
int parse_segment(Layer* layer, char* line, size_t* cursor, size_t line_size, Segment* seg) {
    int res = OK;
    int ia, ib;
    *cursor += 8; // Skip SEGMENT(
    END_IF_NOK(parse_int(line, cursor, line_size, &ia))
    *cursor += 1; // Skip the separator space
    END_IF_NOK(parse_int(line, cursor, line_size, &ib))
    *cursor += 1; // Skip the )

    if (ia >= 0 && ia < layer->size && layer->geoms[ia].type == POINT) {
        seg->a = &(layer->geoms[ia]);
    } else {
        LOG_E("Bad Point Geom index %d", ia);
        return E_PARSE_ISEGMENT_BAD_INDEX;
    }
    if (ib >= 0 && ib < layer->size && layer->geoms[ib].type == POINT) {
        seg->b = &(layer->geoms[ib]);
    } else {
        LOG_E("Bad Point Geom index %d", ib);
        return E_PARSE_ISEGMENT_BAD_INDEX;
    }

    return res;
}

// Parse BEZIER(N N)
// Where N is the index of a Point Geom in the Layer
int parse_bezier(Layer* layer, char* line, size_t* cursor, size_t line_size, Bezier* bez) {
    int res = OK;
    bez->size = 0;

    *cursor += 7; // Skip BEZIER(
    for(int i = 0; (i < MAX_BEZIER_POINT) && (*cursor < line_size) && (line[*cursor - 1] != ')'); i++) {
        int index;
        END_IF_NOK(parse_int(line, cursor, line_size, &index))
        *cursor += 1; // Skip the separator space (or other next char)
        if (index >= 0 && index < layer->size && layer->geoms[index].type == POINT) {
            bez->points[i] = &(layer->geoms[index]);
            bez->size++;
        } else {
            LOG_E("Bad Point Geom index %d", index);
            return E_PARSE_ISEGMENT_BAD_INDEX;
        }
    }

    for (int i = 0; i < BEZIER_LUT_SIZE; i++) {
        bez->lut[i] = bezier(((float)i)/(BEZIER_LUT_SIZE-1), bez);
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
        END_IF_NOK(parse_segment(layer, line, cursor, line_size, &(layer->geoms[layer->size].segment)))
        layer->size += 1;
    } else if (strcmp(wkt_type, "BEZIER") == 0) {
        layer->geoms[layer->size].type = BEZIER;
        END_IF_NOK(parse_bezier(layer, line, cursor, line_size, &(layer->geoms[layer->size].bezier)))
        layer->size += 1;
    } else {
        LOG_E("Unsuported word %s in line %s", wkt_type, line);
        return E_PARSE_UNSUPPORTED;
    }

    return res;
}

/* Signed Distance Functions */

float sign(float s) {
    return (s > 0.0) - (s < 0.0);
}

float length2(Vec2 p) {
    return sqrtf(p.x*p.x + p.y*p.y);
}

Vec2 add2(Vec2 a, Vec2 b) {
    return (Vec2){a.x + b.x, a.y + b.y};
}

Vec2 sub2(Vec2 a, Vec2 b) {
    return (Vec2){a.x - b.x, a.y - b.y};
}

Vec2 mul2(Vec2 a, float s) {
    return (Vec2){a.x * s, a.y * s};
}

float dot2(Vec2 a, Vec2 b) {
    return a.x*b.x + a.y*b.y;
}

float cro2( Vec2 a, Vec2 b ) { 
    return a.x*b.y - a.y*b.x;
}

// Smooth min using the quadratic method. Return the distance and a color mixing value
Vec2 sminq( float a, float b, float k )
{
    float h = 1.0 - min( fabsf(a-b)/(6.0*k), 1.0 );
    float w = h*h*h;
    float m = w*0.5;
    float s = w*k;
    Vec2 ra = {a-s,m};
    Vec2 rb = {b-s,1.0-m};
    return (a<b) ? ra : rb;
}

RichDistance sdMin(RichDistance a, RichDistance b) {
    if (a.d < b.d) {
        return a;
    }
    return b;
}

RichDistance sdSmoothMin(RichDistance a, RichDistance b) {
    Vec2 sd = sminq(a.d, b.d, 1);
    RichDistance rd;
    rd.d = sd.x;
    mix4(rd.rgba, a.rgba, b.rgba, sd.y);
    return rd;
}

RichDistance opRound(RichDistance rd, float r) {
    rd.d -= r; 
    return rd;
}

RichDistance sdPoint(Point p, Point a) {
    RichDistance rd;
    rd.d = length2(sub2(p.v, a.v));
    copy4(rd.rgba, a.rgba);
    return rd;
}

// Exact SDF for segment, from https://iquilezles.org/articles/distfunctions2d/
RichDistance sdSegment(Point p, Geom* ag, Geom* bg ) {
    RichDistance rd;
    Vec2 a = ag->point.v;
    Vec2 b = bg->point.v;
    // Calculate distance
    Vec2 pa = sub2(p.v, a);
    Vec2 ba = sub2(b, a);
    float h = clamp(dot2(pa,ba)/dot2(ba,ba), 0.0, 1.0); // h is the projection of the Point p on segment AB, with value 0 for A, and 1 for B
    rd.d = length2(sub2(pa, mul2(ba, h)));

    // Calculate color gradiant.
    // We want the gradiant to start from the edge of the circle, which add some complexity
    float dab = length2(sub2(b, a));
    float ar = ag->round_r / dab; // Where the color gradiant start for A, on segment AB
    float br = bg->round_r / dab; // Where the color gradiant start for B, on segment BA
    float ch = clamp(h-ar, 0.0, (1-(ar+br))) / (1 - (ar+br)); // h for color, ar become the 0 of ch, and br become the 1
    mix4(rd.rgba, ag->point.rgba, bg->point.rgba, ch);
    
    return rd;
}

Vec2 bezier(float t, Bezier* B) {
    Vec2 res = {0, 0};
    const int n = B->size-1;
    for (int i = 0; i < B->size; i++) {
        res = add2(res, mul2(B->points[i]->point.v, binom_coef[n][i]*powf(1.0-t, n-i)*powf(t, i)));
    }
    return res;
}

RichDistance sdApproximateBezier(Point pos, Bezier* bez) {
    float d = CANVAS_HEIGHT*CANVAS_WIDTH;

    int min_i = 0;
    for (int i = 0; i < BEZIER_LUT_SIZE; i++) {
        float temp_d = length2(sub2(bez->lut[i], pos.v));
        if (temp_d < d) {
            min_i = i;
            d = temp_d;
        }
    }
    if (min_i == 0) {min_i += 2;}
    if (min_i == BEZIER_LUT_SIZE-1) {min_i -= 2;}

    float t[5];
    Vec2 bezier_point[5];
    float dist[5];
    for (int i = 0; i < 5; i++) {
        t[i] = ((float)(min_i+(i-2)))/(BEZIER_LUT_SIZE-1);
        bezier_point[i] = bezier(t[i], bez);
        dist[i] = length2(sub2(pos.v, bezier_point[i]));
    }
    while ((t[4] - t[0]) > 1e-4) {
        int min_i = 0;
        d = dist[0];
        for (int i = 1; i < 5; i++) {
            if (dist[i] < d) {
                d = dist[i];
                min_i = i;
            }
        }
        if (min_i == 0) {min_i = 1;}
        if (min_i == 4) {min_i = 3;}
        t[0] = t[min_i-1];
        t[4] = t[min_i+1];
        t[2] = t[min_i];
        bezier_point[0] = bezier_point[min_i-1];
        bezier_point[4] = bezier_point[min_i+1];
        bezier_point[2] = bezier_point[min_i];
        dist[0] = dist[min_i-1];
        dist[4] = dist[min_i+1];
        dist[2] = dist[min_i];
        
        t[1] = (t[0] + t[2])/2.0;
        t[3] = (t[2] + t[4])/2.0;
        bezier_point[1] = bezier(t[1], bez);
        bezier_point[3] = bezier(t[3], bez);
        dist[1] = length2(sub2(pos.v, bezier_point[1]));
        dist[3] = length2(sub2(pos.v, bezier_point[3]));
    }
    
    RichDistance rd;
    rd.d = d;
    copy4(rd.rgba, bez->points[0]->point.rgba);
    return rd;
}

void sdRenderLayer(Layer layer, float x, float y, float pixel[4], float* distance) {
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
        case BEZIER:
            gd = opRound(sdApproximateBezier(p, &(layer.geoms[i].bezier)), layer.geoms[i].round_r);
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
            d = sdSmoothMin(d, gd);
            break;
        default:
            break;
        }
    }
    *distance = d.d;
    float opacity = clamp(-d.d, 0.0, 1.0); // antialiasing, inside the Geom means 1, more than one pixel away means 0
    copy4(pixel, d.rgba);
    pixel[3] *= opacity;
}

void sdRenderScene(Scene* scene, float x, float y, unsigned char pixel[3], float *distance) {
    *distance = CANVAS_HEIGHT*CANVAS_WIDTH;
    float avgPixel[3] = {0, 0, 0};
    float pixels4[MAX_LAYER][4];
    for (int i = 0; i < scene->size; i++) {
        float d;
        sdRenderLayer(scene->layer[i], x, y, pixels4[i], &d);
        *distance = min(*distance, d);
    }
    for (int i = 0; i < scene->size; i++) {
        avgPixel[0] += pixels4[i][0]*pixels4[i][3];
        avgPixel[1] += pixels4[i][1]*pixels4[i][3];
        avgPixel[2] += pixels4[i][2]*pixels4[i][3];
    }
    avgPixel[0] = clamp(avgPixel[0], 0.0, 1.0);
    avgPixel[1] = clamp(avgPixel[1], 0.0, 1.0);
    avgPixel[2] = clamp(avgPixel[2], 0.0, 1.0);
    pixel[0] = (unsigned char) (avgPixel[2] * 255);
    pixel[1] = (unsigned char) (avgPixel[1] * 255);
    pixel[2] = (unsigned char) (avgPixel[0] * 255);
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
    if (imageFile == NULL) {
        LOG_E("Failed to open output file %s", imageFileName);
        return E_FILE_OPEN;
    }

    unsigned char* fileHeader = create_bitmap_file_header(CANVAS_HEIGHT, stride);
    fwrite(fileHeader, 1, FILE_HEADER_SIZE, imageFile);

    unsigned char* infoHeader = create_bitmap_info_header(CANVAS_HEIGHT, CANVAS_WIDTH);
    fwrite(infoHeader, 1, INFO_HEADER_SIZE, imageFile);

    for (int y = 0; y < CANVAS_HEIGHT; y++) {
        int next_pixel = 0;
        float last_distance = 0;
        for (int x = 0; x < CANVAS_WIDTH; x++) {
            unsigned char pixel[] = {0, 0, 0};
            if (x >= next_pixel) { // Simple optimization, since we know the distance to the next pixel
                sdRenderScene(scene, x, y, pixel, &last_distance);
                next_pixel = x + (int) last_distance;
            } else {
                // Uncomment to see the effect of the optimization. Red means pixels are skipped
                // pixel[2] = (unsigned char) clamp(last_distance, 0, 255);
            }
            fwrite(pixel, BYTES_PER_PIXEL, 1, imageFile);
        }
        fwrite(padding, 1, paddingSize, imageFile);
    }

    fclose(imageFile);
}

int render_file(char* input, char* output) {
    int res = OK;

    Scene scene;
    scene.size = 0;

    char* line = NULL;
    size_t len = 0;
    ssize_t read;

    FILE* fp = fopen(input, "r");
    if (fp == NULL) {
        LOG_E("Failed to open input file %s", input);
        return E_FILE_OPEN;
    }

    while ((read = getline(&line, &len, fp)) != -1) {
        size_t cursor = 0;
        if (line[read-1] == '\n') {line[--read] = '\0';} // Remove LF (insufficient for Windows LFCR)
        if(parse_line(&scene, line, &cursor, read) != OK) {
            LOG_E("Got error %d for line: %s", res, line);
        }
    }

    fclose(fp);
    if (line) {
        free(line);
    }

    res = write_bitmap(&scene, "canvas.bmp");

    return res;
}

int main(int argc, char* argv[]) {
    DIAG = sqrtf(CANVAS_WIDTH*CANVAS_WIDTH + CANVAS_HEIGHT*CANVAS_HEIGHT);

    if (argc != 2) {
        fprintf(stderr, "Usage: %s <inputFile>\n", argv[0]);
        return EXIT_FAILURE;
    }

    render_file(argv[1], "canvas.bmp");

    exit(EXIT_SUCCESS);
}
