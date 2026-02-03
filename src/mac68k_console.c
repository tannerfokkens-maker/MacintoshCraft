/*
 * mac68k_console.c - Simple console window for 68k Macintosh
 */

#ifdef MAC68K_PLATFORM

#include <Quickdraw.h>
#include <Windows.h>
#include <Fonts.h>
#include <Menus.h>
#include <TextEdit.h>
#include <Dialogs.h>
#include <Events.h>
#include <OSUtils.h>
#include <ToolUtils.h>
#include <Memory.h>
#include <SegLoad.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mac68k_console.h"

/* Font constants - Monaco is font ID 4 */
#define kFontMonaco 4

/* Console state */
static WindowPtr g_console_window = NULL;
static TEHandle g_te_handle = NULL;
static int g_should_quit = 0;
static int g_line_count = 0;

/* Maximum lines before we start clearing old content */
#define MAX_CONSOLE_LINES 100

/* Window dimensions */
#define WINDOW_WIDTH 500
#define WINDOW_HEIGHT 320

void console_init(void) {
    Rect window_rect, text_rect;
    Rect screen_rect;

    /* Initialize Toolbox managers */
    InitGraf(&qd.thePort);
    InitFonts();
    InitWindows();
    InitMenus();
    TEInit();
    InitDialogs(NULL);
    InitCursor();

    /* Center window on screen */
    screen_rect = qd.screenBits.bounds;
    SetRect(&window_rect,
            (screen_rect.right - WINDOW_WIDTH) / 2,
            (screen_rect.bottom - WINDOW_HEIGHT) / 3 + 40,
            (screen_rect.right + WINDOW_WIDTH) / 2,
            (screen_rect.bottom + WINDOW_HEIGHT) / 3 + 40);

    /* Create the console window */
    g_console_window = NewWindow(NULL, &window_rect, "\pBareiron Server",
                                  true, documentProc, (WindowPtr)-1L, true, 0L);

    if (g_console_window == NULL) {
        /* Can't create window - just exit */
        ExitToShell();
        return;
    }

    SetPort(g_console_window);

    /* Set up text area with margins */
    text_rect = g_console_window->portRect;
    InsetRect(&text_rect, 4, 4);

    /* Create TextEdit handle for scrolling text */
    g_te_handle = TENew(&text_rect, &text_rect);

    if (g_te_handle != NULL) {
        /* Use Monaco 9 for console look */
        TextFont(kFontMonaco);
        TextSize(9);
        (*g_te_handle)->txFont = kFontMonaco;
        (*g_te_handle)->txSize = 9;
    }

    /* Initial message */
    console_print("Bareiron Server for 68k Macintosh\r");
    console_print("==================================\r\r");
}

void console_poll_events(void) {
    EventRecord event;
    WindowPtr which_window;

    /* Use WaitNextEvent with zero sleep for minimal latency during networking */
    if (WaitNextEvent(everyEvent, &event, 0, NULL)) {
        switch (event.what) {
            case mouseDown:
                switch (FindWindow(event.where, &which_window)) {
                    case inGoAway:
                        if (TrackGoAway(which_window, event.where)) {
                            g_should_quit = 1;
                        }
                        break;
                    case inDrag:
                        DragWindow(which_window, event.where, &qd.screenBits.bounds);
                        break;
                    case inContent:
                        if (which_window != FrontWindow()) {
                            SelectWindow(which_window);
                        }
                        break;
                }
                break;

            case keyDown:
            case autoKey:
                /* Check for Cmd-Q to quit */
                if (event.modifiers & cmdKey) {
                    char key = event.message & charCodeMask;
                    if (key == 'q' || key == 'Q') {
                        g_should_quit = 1;
                    }
                }
                break;

            case updateEvt:
                which_window = (WindowPtr)event.message;
                BeginUpdate(which_window);
                SetPort(which_window);
                EraseRect(&which_window->portRect);
                if (g_te_handle != NULL) {
                    TEUpdate(&which_window->portRect, g_te_handle);
                }
                EndUpdate(which_window);
                break;

            case activateEvt:
                /* Handle window activation */
                break;
        }
    }
}

void console_print(const char *str) {
    int len;
    Rect text_rect;

    if (g_console_window == NULL || g_te_handle == NULL) {
        return;
    }

    SetPort(g_console_window);

    len = strlen(str);
    if (len > 0) {
        /* Count newlines for line tracking */
        const char *p = str;
        while (*p) {
            if (*p == '\r' || *p == '\n') {
                g_line_count++;
            }
            p++;
        }

        /* If too many lines, clear and start fresh */
        if (g_line_count > MAX_CONSOLE_LINES) {
            TESetSelect(0, 32767, g_te_handle);
            TEDelete(g_te_handle);
            g_line_count = 0;
            console_print("[Console cleared]\r\r");
        }

        /* Move to end and insert text */
        TESetSelect(32767, 32767, g_te_handle);
        TEInsert((Ptr)str, len, g_te_handle);

        /* Scroll to show new text */
        text_rect = (*g_te_handle)->viewRect;
        TEPinScroll(0, -10000, g_te_handle);

        /* Force redraw */
        InvalRect(&g_console_window->portRect);
    }
}

void console_printf(const char *fmt, ...) {
    char buffer[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);

    console_print(buffer);
}

int console_should_quit(void) {
    return g_should_quit;
}

#endif /* MAC68K_PLATFORM */
