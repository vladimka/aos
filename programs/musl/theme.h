#ifndef THEME_H
#define THEME_H

// Shared config-file theme loader for GUI programs (musl build).
// Reads sys/config.cfg once (the same file kernel/config.c writes/parses in
// ring 0) and fills a static table. Unknown/duplicate keys are ignored
// (first occurrence wins); missing keys fall back to compile-time defaults.

void theme_load(void);

// Returns the parsed value for `key`, or `fallback` when the key is unknown.
unsigned int theme_color(const char *key, unsigned int fallback);

#endif
