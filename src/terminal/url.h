/*
 * Liu — URL detection in terminal content
 * Detects clickable URLs by scanning cell content around a given position.
 */
#ifndef TERMINAL_URL_H
#define TERMINAL_URL_H

#include "core/types.h"
#include "terminal/terminal.h"

typedef struct {
    i32  start_col, start_row;
    i32  end_col, end_row;
    char url[2048];
} TermURL;

/* Find URL at a given cell position. Returns true if found. */
bool url_detect_at(Terminal *t, i32 col, i32 row, TermURL *result);

/* Check if a position is inside a detected URL */
bool url_contains(const TermURL *url, i32 col, i32 row);

#endif /* TERMINAL_URL_H */
