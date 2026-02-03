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

#endif /* MAC68K_PLATFORM */

#endif /* MAC68K_CONSOLE_H */
