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
    Each line of the file contain one instruction.
*/
#ifndef RENDER_H
#define RENDER_H

#include "sys/types.h"

// Return codes
#define OK 0
#define E_BOUND_REACHED -1
#define E_PARSE_UNSUPPORTED -10
#define E_PARSE_NUMBER -11
#define E_PARSE_ISEGMENT_BAD_INDEX -12
#define E_PARSE_NEED_LAYER -13
#define E_RENDER_INVALID_COORD -30

typedef void (*CallbackMessage)(char*);
typedef void (*CallbackPixel)(int, int, float[3]);
typedef int (CallbackReadLine(char**, size_t*));

extern int read_and_render(size_t canvas_width, size_t canvas_height, CallbackReadLine cb_readline, CallbackPixel cb_pixel, CallbackMessage cb_message);

#endif
