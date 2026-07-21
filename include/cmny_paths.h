#ifndef CMNY_PATHS_H
#define CMNY_PATHS_H

#include <stdbool.h>
#include <stddef.h>

bool cmny_resolve_db_path(const char *override_path, char *out, size_t out_size,
                          char *err, size_t err_size);
bool cmny_resolve_db_path_for_home(const char *override_path, const char *portable_home,
                                   char *out, size_t out_size,
                                   char *err, size_t err_size);
bool cmny_prepare_db_parent(const char *path, char *err, size_t err_size);

#endif
