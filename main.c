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

#define CANVAS_WIDTH 800
#define CANVAS_HEIGHT 400
#define MAX_GEOMS 1000
static double DIAG;

// WKT types
#define POINT 0
#define SEGMENT 1

// Return codes
#define OK 0
#define E_PARSE_NUMBER -10
#define E_UNSUPPORTED_WKT -20
#define E_RENDER_INVALID_COORD -30

typedef struct Point {
    double x;
    double y;
} Point;

typedef struct Segment {
    Point a;
    Point b;
} Segment;

typedef struct Geom {
    char type; // See "WKT types"
    Point point;
    Segment segment;
    double round_r;
} Geom;

// Definitions
int parse_line(char* line, size_t* cursor, size_t line_size, Geom* geo);
// ===

int parse_number(char* str, size_t* cursor, size_t stop, double* number) {
    char num[32];
    size_t start = *cursor;
    for (; *cursor < stop && (*cursor - start) < 32 && str[*cursor] != ' ' && str[*cursor] != ')'; *cursor += 1) {
        num[*cursor - start] = str[*cursor];
    }
    if (str[*cursor] != ' ' && str[*cursor] != ')') {
        fprintf(stderr, "Failed to read number from: %s", str);
        return E_PARSE_NUMBER;
    }
    num[*cursor - start] = '\0';

    *number = atof(num);
    return OK;
}

int parse_point(char* line, size_t* cursor, size_t line_size, Point* point) {
    int res = OK;
    *cursor += 6; // Skip POINT(
    if((res = parse_number(line, cursor, line_size, &(point->x))) != OK) {
        return res;
    }
    *cursor += 1; // Skip the separator space
    if((res = parse_number(line, cursor, line_size, &(point->y))) != OK) {
        return res;
    }
    *cursor += 1; // Skip the )
    point->x *= CANVAS_WIDTH;
    point->y *= CANVAS_HEIGHT;
    return res;
}

int parse_segment(char* line, size_t* cursor, size_t line_size, Geom* geom) {
    int res = OK;
    *cursor += 8; // Skip SEGMENT(
    if ((res = parse_point(line, cursor, line_size, &(geom->segment.a))) != OK) {
        return res;
    }
    *cursor += 1; // Skip the separator space
    if ((res = parse_point(line, cursor, line_size, &(geom->segment.b))) != OK) {
        return res;
    }
    *cursor += 1; // Skip the )
    return res;
}

int parse_round(char* line, size_t* cursor, size_t line_size, Geom* geom) {
    int res = OK;
    *cursor += 6; // Skip ROUND(
    if((res = parse_line(line, cursor, line_size, geom)) != OK) {
        return res;
    }
    *cursor += 1; // Skip the separator space
    if((res = parse_number(line, cursor, line_size, &(geom->round_r))) != OK) {
        return res;
    }
    *cursor += 1; // Skip the )
    geom->round_r *= DIAG;
    return res;
}

int parse_line(char* line, size_t* cursor, size_t line_size, Geom* geo) {
    int res = 0;
    char wkt_type[32];
    size_t wkt_type_size = 0;
    for (wkt_type_size = 0; wkt_type_size < 32 && (*cursor + wkt_type_size) < line_size && line[*cursor + wkt_type_size] != '('; wkt_type_size++) {
        wkt_type[wkt_type_size] = line[*cursor + wkt_type_size];
    }
    wkt_type[wkt_type_size] = '\0';

    if (strcmp(wkt_type, "POINT") == 0) {
        geo->type = POINT;
        if ((res = parse_point(line, cursor, line_size, &(geo->point))) != OK) {
            return res;
        }
    } else if (strcmp(wkt_type, "ROUND") == 0) {
        if ((res = parse_round(line, cursor, line_size, geo)) != OK) {
            return res;
        }
    } else if (strcmp(wkt_type, "SEGMENT") == 0) {
        geo->type = SEGMENT;
        if ((res = parse_segment(line, cursor, line_size, geo)) != OK) {
            return res;
        }
    } else {
        fprintf(stderr, "Unsuported WKT: %s", line);
        return E_UNSUPPORTED_WKT;
    }

    return res;
}

/* Signed Distance Functions, from https://iquilezles.org/articles/distfunctions2d/ */

double length(Point p) {
    return p.x*p.x + p.y*p.y;
}

Point sub(Point a, Point b) {
    Point c = {a.x - b.x, a.y - b.y};
    return c;
}

Point mul(Point a, double s) {
    Point c = {a.x * s, a.y * s};
    return c;
}

double dot(Point a, Point b) {
    return a.x*b.x + a.y*b.y;
}

double min(double a, double b) {
    return a < b ? a : b;
}

// Smooth min using the root method
double smin( double a, double b, float k ) {
    k *= 2.0;
    float x = b-a;
    return 0.5*(a+b-sqrt(x*x+k*k));
}

double max(double a, double b) {
    return a > b ? a : b;
}

double clamp(double v, double vmin, double vmax) {
    return max(vmin, min(vmax, v));
}

double opRound(double d, double r) {
    return d - r;
}

double sdPoint(Point p, Point a) {
    return length(sub(p, a));
}

float sdSegment(Point p, Point a, Point b ) {
    Point pa = sub(p, a);
    Point ba = sub(b, a);
    double h = clamp(dot(pa,ba)/dot(ba,ba), 0.0, 1.0);
    return length(sub(pa, mul(ba, h)));
}

void sdRender(Geom geoms[MAX_GEOMS], size_t size, double x, double y, unsigned char pixel[3]) {
    double d = CANVAS_HEIGHT*CANVAS_WIDTH;
    Point p = {x, y};
    for (size_t i = 0; i < size; i++) {
        switch (geoms[i].type)
        {
        case POINT:
            d = min(d, opRound(sdPoint(p, geoms[i].point), geoms[i].round_r));
            break;
        case SEGMENT:
            d = min(d, opRound(sdSegment(p, geoms[i].segment.a, geoms[i].segment.b), geoms[i].round_r));
            break;
        default:
            break;
        }

        double opacity = clamp(0.5-d, 0, 1);
        if (opacity > 0.5) {
            pixel[0] = (unsigned char) ((i * 2) % 255)*opacity;
            pixel[1] = (unsigned char) ((100 + i) % 255)*opacity;
            pixel[2] = (unsigned char) ((50 / (i + 1)) % 255)*opacity;
            break;
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

int write_bitmap(Geom geoms[MAX_GEOMS], size_t size, char* imageFileName)
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
            sdRender(geoms, size, x, y, pixel);
            fwrite(pixel, BYTES_PER_PIXEL, 1, imageFile);
        }
        fwrite(padding, 1, paddingSize, imageFile);
    }

    fclose(imageFile);
}

int main(void) {
    DIAG = sqrt(CANVAS_WIDTH*CANVAS_WIDTH + CANVAS_HEIGHT*CANVAS_HEIGHT);
    Geom geoms[MAX_GEOMS];

    FILE* fp;
    char* line = NULL;
    size_t len = 0;
    ssize_t read;

    fp = fopen("segments.wkt", "r");
    if (fp == NULL)
        exit(EXIT_FAILURE);

    size_t i = 0;
    while ((read = getline(&line, &len, fp)) != -1 && i < MAX_GEOMS) {
        size_t cursor = 0;
        if(parse_line(line, &cursor, len, &geoms[i]) == OK) {
            i++;
        }
    }

    write_bitmap(geoms, i, "canvas.bmp");

    fclose(fp);
    if (line)
        free(line);
    exit(EXIT_SUCCESS);
}
