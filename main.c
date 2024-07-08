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

#include "stdlib.h"
#include "stdio.h"

#include "render.h"

#define LOG_E(FORMAT, ...) fprintf(stderr, "%s:%d ERROR: " FORMAT "\n", __FILE__, __LINE__, __VA_ARGS__);

#define E_FILE_OPEN -50

void print(char* msg) {
    fprintf(stderr, "%s", msg);
}


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

#define CANVAS_WIDTH 800
#define CANVAS_HEIGHT 800

FILE* imageFile = NULL;
const int widthInBytes = CANVAS_WIDTH * BYTES_PER_PIXEL;
const int paddingSize = (4 - (widthInBytes) % 4) % 4;
const int stride = (widthInBytes) + paddingSize;

int create_bitmap_file(char* imageFileName) {
    imageFile = fopen(imageFileName, "wb");
    if (imageFile == NULL) {
        LOG_E("Failed to open output file %s", imageFileName);
        return E_FILE_OPEN;
    }

    unsigned char* fileHeader = create_bitmap_file_header(CANVAS_HEIGHT, stride);
    fwrite(fileHeader, 1, FILE_HEADER_SIZE, imageFile);

    unsigned char* infoHeader = create_bitmap_info_header(CANVAS_HEIGHT, CANVAS_WIDTH);
    fwrite(infoHeader, 1, INFO_HEADER_SIZE, imageFile);
    return OK;
}

void write_bitmap_pixel(int x, int y, float pixel[3]) {
    if (x >= CANVAS_WIDTH || y >= CANVAS_HEIGHT) {
        return;
    }

    unsigned char data[3];
    data[0] = (unsigned char) (pixel[2] * 255);
    data[1] = (unsigned char) (pixel[1] * 255);
    data[2] = (unsigned char) (pixel[0] * 255);
    fwrite(data, BYTES_PER_PIXEL, 1, imageFile);
    if (x == CANVAS_WIDTH - 1) {
        unsigned char padding[3] = {0, 0, 0};
        fwrite(padding, 1, paddingSize, imageFile);
    }
}

FILE* inputFile = NULL;
int read_instruction_line(char** line, size_t* len) {
    return getline(line, len, inputFile);
}

int render_file(char* input, char* output) {
    int res = OK;

    inputFile = fopen(input, "r");
    if (inputFile == NULL) {
        LOG_E("Failed to open input file %s", input);
        return E_FILE_OPEN;
    }

    res = create_bitmap_file("canvas.bmp");
    if (res == OK) {
        read_and_render(CANVAS_WIDTH, CANVAS_HEIGHT, &read_instruction_line, &write_bitmap_pixel, &print);
        fclose(imageFile);
    }

    fclose(inputFile);

    return res;
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "Usage: %s <inputFile>\n", argv[0]);
        return -1;
    }

    render_file(argv[1], "canvas.bmp");

    exit(OK);
}
