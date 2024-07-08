/*
    Author: Hugo Roussel, unless indicated otherwise.
    License: MIT, with parts in CC BY-SA 4.0.

    Render, using Signed Distance Function, the instruction provided.

    Instruction typically look like:
        LAYER(1)
        POINT(0.61 0.01768)
        POINT(0.06 0.03713) 
        ROUND(0.01 SEGMENT(0 1))
        POINT(-0.5 0.8)
        ROUND(0.01 BEZIER(0 1 3))
*/
#include "stdio.h"
#include "stdarg.h"
#include "stdlib.h"
#include "sys/types.h"
#include "string.h"
#include "math.h"
#include "float.h" // FLT_MAX

#include "render.h"

#define LOG_E(FORMAT, ...) log_printf("%s:%d ERROR: " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__);
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

#define MAX_GEOMS_PER_LAYER 500
#define MAX_LAYER 5
#define BEZIER_LUT_SIZE 31
#define MAX_BEZIER_POINT 11
#define BEZIER_MAX_ITERATIONS 10
#define BEZIER_EPSILON 1e-6
#define SMOOTH_MIN_FACTOR 1.5

// Global rendering parameters set at runtime
static int _canvas_width = 0;
static int _canvas_height = 0;
static float _diag = 0;

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

typedef struct Bbox {
    Vec2 bl; // Bottom Left corner
    Vec2 ur; // Upper Right corner
} Bbox;

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
    Bbox bbox;
};

struct Layer {
    int fusion; // See "Fusion types"
    Geom geoms[MAX_GEOMS_PER_LAYER];
    size_t size;
    Bbox bbox;
};

struct Scene {
    Layer layer[MAX_LAYER];
    size_t size;
};

struct RichDistance {
    float d;
    float rgba[4];
};
#define DEFAULT_RD (RichDistance){FLT_MAX, {0, 0, 0, 0}}

// function Definitions
static int parse_line(Scene* scene, char* line, size_t* cursor, size_t line_size);
// Vec2 quadraticBezier(float t, Vec2 A, Vec2 B, Vec2 C);
static Vec2 bezier(float t, Bezier* B);
// ===

CallbackMessage message_callback = NULL;
static void log_printf(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char msg[512];
    vsnprintf(msg, 512, fmt, args);
    message_callback(msg);
}

static int parse_number(char* str, size_t* cursor, size_t stop, float* number) {
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

static int parse_int(char* str, size_t* cursor, size_t stop, int* number) {
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
static int parse_color(char* line, size_t* cursor, size_t line_size, Point* point) {
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
static int parse_point(char* line, size_t* cursor, size_t line_size, Point* point) {
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
    point->v.x *= _canvas_width;
    point->v.y *= _canvas_height;

    return res;
}

// Parse SEGMENT(N N)
// Where N is the index of a Point Geom in the Layer
static int parse_segment(Layer* layer, char* line, size_t* cursor, size_t line_size, Segment* seg) {
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
static int parse_bezier(Layer* layer, char* line, size_t* cursor, size_t line_size, Bezier* bez) {
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
static int parse_round(Scene* scene, char* line, size_t* cursor, size_t line_size, Geom* geom) {
    int res = OK;
    *cursor += 6; // Skip ROUND(
    END_IF_NOK(parse_number(line, cursor, line_size, &(geom->round_r)))
    *cursor += 1; // Skip the separator space
    END_IF_NOK(parse_line(scene, line, cursor, line_size))
    *cursor += 1; // Skip the )
    geom->round_r *= _diag;

    geom->bbox.bl.x -= (ceilf(geom->round_r) + 1);
    geom->bbox.bl.y -= (ceilf(geom->round_r) + 1);
    geom->bbox.ur.x += (ceilf(geom->round_r) + 1);
    geom->bbox.ur.y += (ceilf(geom->round_r) + 1);

    return res;
}

// Parse LAYER(N)
// Where N can be any of "Fusion types"
static int parse_layer(Scene* scene, char* line, size_t* cursor, size_t line_size) {
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

static void set_bbox_point(Geom* g) {
    g->bbox.bl = g->point.v;
    g->bbox.ur = g->point.v;
}

static void set_bbox_segment(Geom* g) {
    g->bbox.bl.x = min(g->segment.a->point.v.x, g->segment.b->point.v.x);
    g->bbox.bl.y = min(g->segment.a->point.v.y, g->segment.b->point.v.y);
    g->bbox.ur.x = max(g->segment.a->point.v.x, g->segment.b->point.v.x);
    g->bbox.ur.y = max(g->segment.a->point.v.y, g->segment.b->point.v.y);
}

static void set_bbox_bezier(Geom* g) {
    g->bbox.bl = g->bezier.points[0]->point.v;
    g->bbox.ur = g->bezier.points[0]->point.v;
    for (size_t i = 1; i < g->bezier.size; i++) {
        g->bbox.bl.x = min(g->bbox.bl.x, g->bezier.points[i]->point.v.x);
        g->bbox.bl.y = min(g->bbox.bl.y, g->bezier.points[i]->point.v.y);
        g->bbox.ur.x = max(g->bbox.ur.x, g->bezier.points[i]->point.v.x);
        g->bbox.ur.y = max(g->bbox.ur.y, g->bezier.points[i]->point.v.y);
    }
}

static int parse_line(Scene* scene, char* line, size_t* cursor, size_t line_size) {
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
        set_bbox_point(&(layer->geoms[layer->size]));
        layer->size += 1;
    } else if (strcmp(wkt_type, "SEGMENT") == 0) {
        layer->geoms[layer->size].type = SEGMENT;
        END_IF_NOK(parse_segment(layer, line, cursor, line_size, &(layer->geoms[layer->size].segment)))
        set_bbox_segment(&(layer->geoms[layer->size]));
        layer->size += 1;
    } else if (strcmp(wkt_type, "BEZIER") == 0) {
        layer->geoms[layer->size].type = BEZIER;
        END_IF_NOK(parse_bezier(layer, line, cursor, line_size, &(layer->geoms[layer->size].bezier)))
        set_bbox_bezier(&(layer->geoms[layer->size]));
        layer->size += 1;
    } else {
        LOG_E("Unsuported word %s in line %s", wkt_type, line);
        return E_PARSE_UNSUPPORTED;
    }

    return res;
}

/* Signed Distance Functions */

static inline float sign(float s) {
    return (s > 0.0) - (s < 0.0);
}

static inline Vec2 max2(Vec2 a, Vec2 b) {
    return (Vec2){max(a.x, b.x), max(a.y, b.y)};
}

static inline Vec2 add2(Vec2 a, Vec2 b) {
    return (Vec2){a.x + b.x, a.y + b.y};
}

static inline Vec2 sub2(Vec2 a, Vec2 b) {
    return (Vec2){a.x - b.x, a.y - b.y};
}

static inline Vec2 mul2(Vec2 a, float s) {
    return (Vec2){a.x * s, a.y * s};
}

static inline float dot2(Vec2 a, Vec2 b) {
    return a.x*b.x + a.y*b.y;
}

static inline float cro2( Vec2 a, Vec2 b ) { 
    return a.x*b.y - a.y*b.x;
}

static inline float length2(Vec2 p) {
    return sqrtf(p.x*p.x + p.y*p.y);
}

static inline float distance2(Vec2 a, Vec2 b) {
    return length2(sub2(a, b));
}

static float squaredDistance2(Vec2 a, Vec2 b) {
    Vec2 diff = sub2(a, b);
    return dot2(diff, diff);
}

static inline Vec2 lerp2(Vec2 a, Vec2 b, float t) {
    return (Vec2){mix(a.x, b.x, t), mix(a.y, b.y, t)};
}

// Smooth min using the quadratic method. Return the distance and a color mixing value
static Vec2 sminq( float a, float b, float k )
{
    float h = 1.0 - min( fabsf(a-b)/(6.0*k), 1.0 );
    float w = h*h*h;
    float m = w*0.5;
    float s = w*k;
    Vec2 ra = {a-s,m};
    Vec2 rb = {b-s,1.0-m};
    return (a<b) ? ra : rb;
}

static inline RichDistance sdMin(RichDistance a, RichDistance b) {
    return a.d < b.d ? a : b;
}

static RichDistance sdSmoothMin(RichDistance a, RichDistance b) {
    Vec2 sd = sminq(a.d, b.d, SMOOTH_MIN_FACTOR);
    RichDistance rd;
    rd.d = sd.x;
    mix4(rd.rgba, a.rgba, b.rgba, sd.y);
    return rd;
}

static inline RichDistance opRound(RichDistance rd, float r) {
    rd.d -= r; 
    return rd;
}

static RichDistance sdPoint(Point p, Point a) {
    RichDistance rd;
    rd.d = length2(sub2(p.v, a.v));
    copy4(rd.rgba, a.rgba);
    return rd;
}

// Exact SDF for segment, from https://iquilezles.org/articles/distfunctions2d/
static RichDistance sdSegment(Point p, Geom* ag, Geom* bg ) {
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

// Bezier using De Casteljau's algorithm
static Vec2 bezier(float t, Bezier* B) {
    Vec2 temp[MAX_BEZIER_POINT];
    
    for (size_t i = 0; i < B->size; i++) {
        temp[i] = B->points[i]->point.v;
    }
    
    for (size_t r = 1; r < B->size; r++) {
        for (size_t i = 0; i < B->size - r; i++) {
            temp[i] = lerp2(temp[i], temp[i+1], t);
        }
    }
    
    return temp[0];
}

// Derivative of the Bezier curve
static Vec2 bezier_derivative(float t, Bezier* B) {
    Vec2 temp[MAX_BEZIER_POINT - 1];
    
    for (size_t i = 0; i < B->size - 1; i++) {
        temp[i].x = B->size * (B->points[i+1]->point.v.x - B->points[i]->point.v.x);
        temp[i].y = B->size * (B->points[i+1]->point.v.y - B->points[i]->point.v.y);
    }
    
    for (size_t r = 1; r < B->size - 1; r++) {
        for (size_t i = 0; i < B->size - r - 1; i++) {
            temp[i] = lerp2(temp[i], temp[i+1], t);
        }
    }
    
    return temp[0];
}

static RichDistance sdApproximateBezier(Point pos, Bezier* bez) {
    float min_distance_sq = FLT_MAX;

    // Initial subdivision to find a good starting point
    int min_i = 0;
    for (int i = 0; i < BEZIER_LUT_SIZE; i++) {
        float distance_sq = squaredDistance2(bez->lut[i], pos.v);
        
        if (distance_sq < min_distance_sq) {
            min_distance_sq = distance_sq;
            min_i = i;
        }
    }
    float min_t = ((float)min_i)/(BEZIER_LUT_SIZE-1);

    // Refine using Newton's method
    for (int i = 0; i < BEZIER_MAX_ITERATIONS; i++) {
        Vec2 point = bezier(min_t, bez);
        Vec2 derivative = bezier_derivative(min_t, bez);
        Vec2 diff = sub2(point, pos.v);
        
        float numerator = dot2(diff, derivative);
        float denominator = dot2(derivative, derivative);
        
        if (fabsf(numerator) < BEZIER_EPSILON * denominator) {
            break;  // We've converged
        }
        
        float t_new = min_t - numerator / denominator;
        
        if (t_new < 0.0f) {
            min_t = 0.0f;
            break;
        } else if (t_new > 1.0f) {
            min_t = 1.0f;
            break;
        }
        
        min_t = t_new;
    }

    Vec2 closest_point = bezier(min_t, bez);
    float d = distance2(closest_point, pos.v);
    
    RichDistance rd;
    rd.d = d;
    copy4(rd.rgba, bez->points[0]->point.rgba);
    return rd;
}

float distanceBbox(Bbox bbox, float x, float y) {
    if (x < bbox.bl.x) {
        return (bbox.bl.x - x);
    }
    if (y < bbox.bl.y) {
        return (bbox.bl.y - y);
    }
    if (x > bbox.ur.x) {
        return (x - bbox.ur.x);
    }
    if (y > bbox.ur.y) {
        return (y - bbox.ur.y);
    }
    return -1;
}

static void sdRenderLayer(Layer* layer, float x, float y, float pixel[4], float* distance) {
    pixel[0] = 0;
    pixel[1] = 0;
    pixel[2] = 0;
    pixel[3] = 0;
    float dbb = distanceBbox(layer->bbox, x, y);
    if (dbb > 0) {
        *distance = dbb;
        return;
    }
    
    RichDistance d = DEFAULT_RD;
    Point p = {x, y};
    for (size_t i = 0; i < layer->size; i++) {
        RichDistance gd = DEFAULT_RD;
        switch (layer->geoms[i].type)
        {
        case POINT:
            gd = opRound(sdPoint(p, layer->geoms[i].point), layer->geoms[i].round_r);
            break;
        case SEGMENT:
            dbb = distanceBbox(layer->geoms[i].bbox, x, y);
            if (dbb-(SMOOTH_MIN_FACTOR*5) <= 0) {
                gd = opRound(sdSegment(p, layer->geoms[i].segment.a, layer->geoms[i].segment.b), layer->geoms[i].round_r);
            } else {
                gd.d = dbb;
            }
            break;
        case BEZIER:
            dbb = distanceBbox(layer->geoms[i].bbox, x, y);
            if (dbb-(SMOOTH_MIN_FACTOR*5) <= 0) {
                gd = opRound(sdApproximateBezier(p, &(layer->geoms[i].bezier)), layer->geoms[i].round_r);
            } else {
                gd.d = dbb;
            }
            break;
        default:
            break;
        }

        switch (layer->fusion)
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
    pixel[3] = opacity;
}

static void sdRenderScene(Scene* scene, float x, float y, float* avgPixel, float *distance) {
    *distance = FLT_MAX;
    float pixels4[MAX_LAYER][4];
    for (int i = 0; i < scene->size; i++) {
        float d;
        sdRenderLayer(&(scene->layer[i]), x, y, pixels4[i], &d);
        *distance = min(*distance, d);
    }
    avgPixel[0] = 0.0;
    avgPixel[1] = 0.0;
    avgPixel[2] = 0.0;
    for (int i = 0; i < scene->size; i++) {
        avgPixel[0] += pixels4[i][0]*pixels4[i][3];
        avgPixel[1] += pixels4[i][1]*pixels4[i][3];
        avgPixel[2] += pixels4[i][2]*pixels4[i][3];
    }
    avgPixel[0] = clamp(avgPixel[0], 0.0, 1.0);
    avgPixel[1] = clamp(avgPixel[1], 0.0, 1.0);
    avgPixel[2] = clamp(avgPixel[2], 0.0, 1.0);
}

/* === */

// Use the read_line callback to read instructions one by one, and parse them into the scene
extern int read_scene(Scene* scene, size_t canvas_width, size_t canvas_height, int (read_line(char**, size_t*))) {
    int res = OK;

    _canvas_width = canvas_width;
    _canvas_height = canvas_height;
    _diag = sqrtf(canvas_width*canvas_width + canvas_height*canvas_height);

    size_t len = 512;
    int read = 0;
    char* line = malloc(sizeof(unsigned char) * len);
    while ((read = read_line(&line, &len)) > 0) {
        size_t cursor = 0;
        if (line[read-1] == '\n') {line[--read] = '\0';} // Remove LF
        if (line[read-1] == '\r') {line[--read] = '\0';} // Remove CR
        if(parse_line(scene, line, &cursor, read) != OK) {
            LOG_E("Got error %d for line: %s", res, line);
        }
    }
    if(line) {free(line);}

    // Compute Layer bbox
    for (size_t i = 0; i < scene->size; i++) {
        Layer* l = &(scene->layer[i]);
        Geom g = l->geoms[0];
        l->bbox.bl = g.bbox.bl;
        l->bbox.ur = g.bbox.ur;
        for (size_t j = 1; j < l->size; j++) {
            g = l->geoms[j];
            l->bbox.bl.x = min(l->bbox.bl.x, g.bbox.bl.x);
            l->bbox.bl.y = min(l->bbox.bl.y, g.bbox.bl.y);
            l->bbox.ur.x = max(l->bbox.ur.x, g.bbox.ur.x);
            l->bbox.ur.y = max(l->bbox.ur.y, g.bbox.ur.y);
        }
    }
    return res;
}

// Render the scene, and write the resulting pixel one by one using the handle_pixel callback
extern void render_canvas(Scene* scene, size_t canvas_width, size_t canvas_height, void (*handle_pixel)(int, int, float[3])) {
    for (size_t y = 0; y < canvas_height; y++) {
        size_t next_pixel = 0;
        float last_distance = 0;
        for (size_t x = 0; x < canvas_width; x++) {
            float pixel[3] = {0, 0, 0};
            if (x >= next_pixel) { // Simple optimization, since we know the distance to the next pixel
                sdRenderScene(scene, x, y, pixel, &last_distance);
                next_pixel = x + (int)clamp(last_distance, 0, canvas_width);
            } else {
                // Uncomment to see the distance as red gradiant.
                // With the optimization, red streak means a lot of pixels are skipped.
                // pixel[0] = clamp(last_distance / _diag, 0, 1);
            }
            handle_pixel(x, y, pixel);
        }
    }
}

extern int read_and_render(size_t canvas_width, size_t canvas_height, CallbackReadLine cb_readline, CallbackPixel cb_pixel, CallbackMessage cb_message) {
    message_callback = cb_message;
    int res = OK;

    Scene scene;
    scene.size = 0;

    res = read_scene(&scene, canvas_width, canvas_height, cb_readline);
    render_canvas(&scene, canvas_width, canvas_height, cb_pixel);

    return res;
}
