// File paths and whole-file reading and writing.

#ifndef JLB_PATH_H
#define JLB_PATH_H

#include "jlb.h"

char *path_join(const char *dir, const char *name);
char *path_parent(const char *p);
char *path_file_name(const char *p);
char *path_stem(const char *p);
char *path_canonical(const char *p);
bool path_is_file(const char *p);
bool path_exists(const char *p);
char *os_error(int err);
bool read_file(const char *path, char **text, size_t *len, char **error);
bool write_file(const char *path, const char *text, size_t len, char **error);

#endif
