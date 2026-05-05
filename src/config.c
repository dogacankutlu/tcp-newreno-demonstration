#include "config.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == '\n' || end[-1] == '\r' ||
                       end[-1] == ' '  || end[-1] == '\t')) {
        end--;
    }
    *end = '\0';
    return s;
}

int config_load(const char *path, node_config_t *out) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "config: cannot open %s\n", path);
        return -1;
    }

    memset(out, 0, sizeof(*out));
    out->port = -1;

    char line[MAX_LINE];
    int lineno = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = trim(line);
        if (*p == '\0' || *p == '#') continue;

        char key[32];
        if (sscanf(p, "%31s", key) != 1) continue;

        if (strcmp(key, "name") == 0) {
            sscanf(p, "%*s %7s", out->name);
        } else if (strcmp(key, "port") == 0) {
            sscanf(p, "%*s %d", &out->port);
        } else if (strcmp(key, "neighbor") == 0) {
            if (out->n_neighbors >= MAX_NEIGHBORS) {
                fprintf(stderr, "config: too many neighbors at line %d\n", lineno);
                fclose(f);
                return -1;
            }
            neighbor_t *n = &out->neighbors[out->n_neighbors];
            int got = sscanf(p, "%*s %7s %63s %d %d",
                             n->name, n->host, &n->port, &n->cost);
            if (got != 4) {
                fprintf(stderr, "config: bad neighbor line %d: %s\n", lineno, p);
                fclose(f);
                return -1;
            }
            out->n_neighbors++;
        } else {
            fprintf(stderr, "config: unknown key '%s' at line %d\n", key, lineno);
        }
    }

    fclose(f);

    if (out->name[0] == '\0' || out->port < 0) {
        fprintf(stderr, "config: missing name or port\n");
        return -1;
    }
    return 0;
}
