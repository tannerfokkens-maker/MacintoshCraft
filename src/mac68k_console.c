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
#include <Devices.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "mac68k_console.h"
#include "profiler.h"

/* Font constants - Monaco is font ID 4 */
#define kFontMonaco 4

/* Menu IDs */
#define MENU_APPLE 128
#define MENU_FILE  129
#define MENU_DEBUG 130

/* Menu item IDs */
#define ITEM_ABOUT          1

#define ITEM_FILE_QUIT      1

#define ITEM_DEBUG_PROFILE  1
#define ITEM_DEBUG_SAVE     2
#define ITEM_DEBUG_RESET    3

/* Console state */
static WindowPtr g_console_window = NULL;
static TEHandle g_te_handle = NULL;
static int g_should_quit = 0;
static int g_line_count = 0;

/* Menu handles */
static MenuHandle g_apple_menu = NULL;
static MenuHandle g_file_menu = NULL;
static MenuHandle g_debug_menu = NULL;

/* Maximum lines before we start clearing old content */
#define MAX_CONSOLE_LINES 100

/* Window dimensions */
#define WINDOW_WIDTH 500
#define WINDOW_HEIGHT 320

/* Forward declarations */
static void setup_menus(void);
static void handle_menu_choice(long menu_choice);

/* Set up the menu bar */
static void setup_menus(void) {
    /* Create Apple menu */
    g_apple_menu = NewMenu(MENU_APPLE, "\p\024"); /* Apple symbol */
    AppendMenu(g_apple_menu, "\pAbout Bareiron...");
    AppendMenu(g_apple_menu, "\p(-");  /* Separator */
    AppendResMenu(g_apple_menu, 'DRVR');  /* Add desk accessories */
    InsertMenu(g_apple_menu, 0);

    /* Create File menu */
    g_file_menu = NewMenu(MENU_FILE, "\pFile");
    AppendMenu(g_file_menu, "\pQuit/Q");
    InsertMenu(g_file_menu, 0);

    /* Create Debug menu */
    g_debug_menu = NewMenu(MENU_DEBUG, "\pDebug");
    AppendMenu(g_debug_menu, "\pEnable Profiling/P");
    AppendMenu(g_debug_menu, "\pSave Report/R");
    AppendMenu(g_debug_menu, "\pReset Stats");
    InsertMenu(g_debug_menu, 0);

    DrawMenuBar();
}

/* Handle menu selection */
static void handle_menu_choice(long menu_choice) {
    short menu_id = HiWord(menu_choice);
    short item_id = LoWord(menu_choice);

    switch (menu_id) {
        case MENU_APPLE:
            if (item_id == ITEM_ABOUT) {
                /* Show simple about message */
                console_print("\r--- About Bareiron ---\r");
                console_print("Minecraft server for 68k Mac\r");
                console_print("Protocol 772 (1.21.8)\r\r");
            } else {
                /* Handle desk accessories */
                Str255 da_name;
                GetMenuItemText(g_apple_menu, item_id, da_name);
                OpenDeskAcc(da_name);
            }
            break;

        case MENU_FILE:
            if (item_id == ITEM_FILE_QUIT) {
                g_should_quit = 1;
            }
            break;

        case MENU_DEBUG:
            switch (item_id) {
                case ITEM_DEBUG_PROFILE:
                    prof_toggle();
                    /* Update checkmark */
                    CheckItem(g_debug_menu, ITEM_DEBUG_PROFILE, prof_is_enabled());
                    break;
                case ITEM_DEBUG_SAVE:
                    prof_save_report();
                    break;
                case ITEM_DEBUG_RESET:
                    prof_reset();
                    console_print("Profiler stats reset\r");
                    break;
            }
            break;
    }

    HiliteMenu(0);  /* Unhighlight menu title */
}

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

    /* Set up menus after window is created */
    setup_menus();

    /* Initialize profiler */
    prof_init();
}

void console_poll_events(void) {
    EventRecord event;
    WindowPtr which_window;
    long menu_choice;

    /* Use WaitNextEvent with zero sleep for minimal latency during networking */
    if (WaitNextEvent(everyEvent, &event, 0, NULL)) {
        switch (event.what) {
            case mouseDown:
                switch (FindWindow(event.where, &which_window)) {
                    case inMenuBar:
                        menu_choice = MenuSelect(event.where);
                        if (menu_choice != 0) {
                            handle_menu_choice(menu_choice);
                        }
                        break;
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
                /* Handle command key shortcuts via MenuKey */
                if (event.modifiers & cmdKey) {
                    char key = event.message & charCodeMask;
                    menu_choice = MenuKey(key);
                    if (menu_choice != 0) {
                        handle_menu_choice(menu_choice);
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
