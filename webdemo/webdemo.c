/*
    Author: Hugo Roussel, unless indicated otherwise.
    License: MIT, with parts in CC BY-SA 4.0.

    Emscripten wrapper for render.c
*/

#include "emscripten.h"
#include "sys/types.h"
#include "stdlib.h"
#include "string.h"

#include "../render.h"

int version() {return 6;}

void print(char* msg) {
    EM_ASM({
        console.log(UTF8ToString($0));
    }, msg);
}

void printi(int i) {
    EM_ASM({
        console.log($0);
    }, i);
}

char* instructions_buffer = NULL;
size_t cursor = 0;

unsigned char* pixels_buffer = NULL;
size_t canvas_width = 0;
size_t canvas_height= 0;

int read_line(char** line, size_t* len) {
    if ((cursor > 0) && (instructions_buffer[cursor-1] == '\0')) {
        return 0;
    }
    size_t i = 0;
    for(i = 0; (instructions_buffer[cursor] != '\n') && (instructions_buffer[cursor] != '\0') && (i < *len); i++) {
        (*line)[i] = instructions_buffer[cursor++];
    }
    cursor++;
    if (i < *len) {
        (*line)[i] = '\0';
    } else {
        (*line)[*len-1] = '\0';
    }
    return i;
}

void handle_pixel(int x, int y, float pixel[3]) {
    int i = ((int)y*canvas_width) + ((int)x%canvas_width);
    pixels_buffer[i*4 + 0] = pixel[0]*255;
    pixels_buffer[i*4 + 1] = pixel[1]*255;
    pixels_buffer[i*4 + 2] = pixel[2]*255;
    pixels_buffer[i*4 + 3] = 255;
}

void free_instructions() {
    if (instructions_buffer) {
        free(instructions_buffer);
    }
    instructions_buffer = NULL;
    cursor = 0;
}

int load_instructions(char* str) {
    free_instructions();
    int len = strlen(str) + 1;
    instructions_buffer = malloc(sizeof(char) * len);
    if (instructions_buffer == NULL) {
        return -1;
    }
    strcpy(instructions_buffer, str);
    cursor = 0;
    return 0;
}

void destroy_result_buffer() {
    if (pixels_buffer){
        free(pixels_buffer);
    }
    pixels_buffer = NULL;
}

unsigned char* create_result_buffer(int width, int height) {
    destroy_result_buffer();
    canvas_height = height;
    canvas_width = width;
    pixels_buffer = malloc(width * height * 4 * sizeof(unsigned char));
    return pixels_buffer;
}

int render() {
    return read_and_render(canvas_width, canvas_height, &read_line, &handle_pixel, &print);
}
