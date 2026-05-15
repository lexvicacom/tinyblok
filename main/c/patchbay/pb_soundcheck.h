#ifndef PB_SOUNDCHECK_H
#define PB_SOUNDCHECK_H

#include <stdbool.h>

// CLI soundcheck knobs; input is always SUBJECT|payload rows on stdin.
typedef struct pb_soundcheck_options {
    bool label;
} pb_soundcheck_options;

int pb_soundcheck_run(const char *path, pb_soundcheck_options opts);

#endif
