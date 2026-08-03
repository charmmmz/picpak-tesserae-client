// test_rest_button.c — host unit test for the pure /frame button-query helpers.
// cc firmware/test/test_rest_button.c -Ifirmware/main -o /tmp/t && /tmp/t
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 varanu5 <https://github.com/varanu5>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "rest_button.h"

int main(void) {
    // Only "refresh" drops If-None-Match; navigation buttons keep it.
    assert(rest_button_clears_etag("refresh"));
    assert(!rest_button_clears_etag("right"));
    assert(!rest_button_clears_etag(""));
    assert(!rest_button_clears_etag(NULL));

    char url[256];
    size_t base = (size_t)snprintf(url, sizeof(url), "http://h/api/v1/device/d/frame");

    // Navigation button appends both params.
    size_t n = rest_button_query(url, base, sizeof(url), "right", 7);
    assert(strcmp(url, "http://h/api/v1/device/d/frame?button=right&button_event_id=7") == 0);
    assert(n == strlen(url));

    // Refresh builds the same shape with its own name.
    base = (size_t)snprintf(url, sizeof(url), "http://h/api/v1/device/d/frame");
    rest_button_query(url, base, sizeof(url), "refresh", 42);
    assert(strcmp(url, "http://h/api/v1/device/d/frame?button=refresh&button_event_id=42") == 0);

    // NULL / empty button -> URL untouched, returns the original length.
    base = (size_t)snprintf(url, sizeof(url), "http://h/frame");
    assert(rest_button_query(url, base, sizeof(url), NULL, 1) == base);
    assert(strcmp(url, "http://h/frame") == 0);
    assert(rest_button_query(url, base, sizeof(url), "", 1) == base);
    assert(strcmp(url, "http://h/frame") == 0);

    // No room to append -> untouched, still NUL-terminated at the base length.
    char tiny[20];
    size_t tbase = (size_t)snprintf(tiny, sizeof(tiny), "http://h/frame");
    assert(rest_button_query(tiny, tbase, sizeof(tiny), "right", 9) == tbase);
    assert(strcmp(tiny, "http://h/frame") == 0);

    printf("test_rest_button: ok\n");
    return 0;
}
