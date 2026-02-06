/*
 * mac68k_console.h - Simple console window for 68k Macintosh
 */

#ifndef MAC68K_CONSOLE_H
#define MAC68K_CONSOLE_H

#ifdef MAC68K_PLATFORM

/* Initialize the console window and Toolbox managers */
void console_init(void);

/* Process Mac events (call periodically to keep UI responsive) */
void console_poll_events(void);

/* Print a string to the console window */
void console_print(const char *str);

/* Print a formatted string to the console window */
void console_printf(const char *fmt, ...);

/* Check if the application should quit */
int console_should_quit(void);

/* Get the configured chunk cache size (in entries) */
int console_get_cache_size(void);

/* Get the mob interpolation setting (0=disabled, 1=enabled) */
int console_get_mob_interpolation(void);

/* Save preferences to disk */
void console_save_prefs(void);

/* Load preferences from disk */
void console_load_prefs(void);

#endif /* MAC68K_PLATFORM */

#endif /* MAC68K_CONSOLE_H */
