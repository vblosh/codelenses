#ifndef FIXTURES_HEADERS_H
#define FIXTURES_HEADERS_H

#include "common.h"
#include <stddef.h>

#define BUFFER_CAPACITY 2048
#define CLAMP(x, lo, hi) (((x) < (lo)) ? (lo) : (((x) > (hi)) ? (hi) : (x)))

typedef unsigned long item_id_t;
typedef struct Context Context;

struct Point {
    int x;
    int y;
};

struct Opaque;

enum Status { STATUS_OK = 0, STATUS_ERROR = 1, STATUS_PENDING = 2 };

extern int g_header_errno;

int init_point(struct Point* pt, int x, int y);
void destroy_context(Context* ctx);

#endif
