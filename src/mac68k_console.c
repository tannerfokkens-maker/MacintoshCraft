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
#include <Folders.h>
#include <Files.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mac68k_console.h"
#include "mac68k_net.h"
#include "profiler.h"
#include "globals.h"

/* Font constants - Monaco is font ID 4 */
#define kFontMonaco 4

/* Menu IDs */
#define MENU_APPLE  128
#define MENU_FILE   129
#define MENU_SERVER 130
#define MENU_DEBUG  131

/* Menu item IDs */
#define ITEM_ABOUT          1

#define ITEM_FILE_QUIT      1

#define ITEM_SERVER_VD1     1
#define ITEM_SERVER_VD2     2
#define ITEM_SERVER_VD3     3
#define ITEM_SERVER_VD4     4
#define ITEM_SERVER_SEP1    5  /* Separator */
#define ITEM_SERVER_CACHE   6  /* Set Cache Size... */
#define ITEM_SERVER_SEP2    7  /* Separator */
#define ITEM_SERVER_INTERP  8  /* Smooth Mob Movement */

#define ITEM_DEBUG_PROFILE  1
#define ITEM_DEBUG_SAVE     2
#define ITEM_DEBUG_RESET    3
#define ITEM_DEBUG_SEP1     4  /* Separator */
#define ITEM_DEBUG_USE_OT   5
#define ITEM_DEBUG_USE_TCP  6
#define ITEM_DEBUG_SEP2     7  /* Separator */
#define ITEM_DEBUG_RESTART  8

/* Console state */
static WindowPtr g_console_window = NULL;
static TEHandle g_te_handle = NULL;
static int g_should_quit = 0;
static int g_line_count = 0;

/* Configuration (saved to preferences) */
static long g_cache_size_kb = 1024;  /* Default: 1024 KB (1MB) */
static int g_mob_interpolation = 1;  /* Default: enabled */

/* Menu handles */
static MenuHandle g_apple_menu = NULL;
static MenuHandle g_file_menu = NULL;
static MenuHandle g_server_menu = NULL;
static MenuHandle g_debug_menu = NULL;

/* Maximum lines before we start clearing old content */
#define MAX_CONSOLE_LINES 100

/* Window dimensions */
#define WINDOW_WIDTH 500
#define WINDOW_HEIGHT 320

/* Forward declarations */
static void setup_menus(void);
static void handle_menu_choice(long menu_choice);
static void update_view_distance_checkmarks(void);
static void update_net_stack_checkmarks(void);
static void show_cache_size_dialog(void);

/* Update checkmarks on view distance menu items */
static void update_view_distance_checkmarks(void) {
    CheckItem(g_server_menu, ITEM_SERVER_VD1, view_distance == 1);
    CheckItem(g_server_menu, ITEM_SERVER_VD2, view_distance == 2);
    CheckItem(g_server_menu, ITEM_SERVER_VD3, view_distance == 3);
    CheckItem(g_server_menu, ITEM_SERVER_VD4, view_distance == 4);
}

/* Update checkmarks on network stack menu items */
static void update_net_stack_checkmarks(void) {
    int selected_ot = net_get_selected_stack();
    CheckItem(g_debug_menu, ITEM_DEBUG_USE_OT, selected_ot);
    CheckItem(g_debug_menu, ITEM_DEBUG_USE_TCP, !selected_ot);
}

/* Show dialog for setting cache size in KB */
static void show_cache_size_dialog(void) {
    WindowPtr dialog;
    Rect dialog_rect, input_rect, ok_rect, cancel_rect;
    TEHandle te;
    char buffer[32];
    EventRecord event;
    int done = 0;
    int accepted = 0;
    Point mouse;

    /* Calculate dialog position (centered) */
    Rect screen = qd.screenBits.bounds;
    SetRect(&dialog_rect,
            (screen.right - 300) / 2,
            (screen.bottom - 120) / 2,
            (screen.right + 300) / 2,
            (screen.bottom + 120) / 2);

    /* Create a simple window as dialog */
    dialog = NewWindow(NULL, &dialog_rect, "\pSet Cache Size",
                       true, dBoxProc, (WindowPtr)-1L, false, 0L);

    if (dialog == NULL) {
        console_print("Could not create dialog\r");
        return;
    }

    SetPort(dialog);
    TextFont(0);
    TextSize(12);

    /* Draw prompt */
    MoveTo(20, 25);
    DrawString("\pEnter cache size in KB:");

    /* Input field area */
    SetRect(&input_rect, 20, 35, 200, 55);
    FrameRect(&input_rect);
    InsetRect(&input_rect, 3, 3);

    /* Create TextEdit for input */
    te = TENew(&input_rect, &input_rect);
    if (te == NULL) {
        DisposeWindow(dialog);
        console_print("Could not create text field\r");
        return;
    }

    /* Set initial value */
    snprintf(buffer, sizeof(buffer), "%ld", g_cache_size_kb);
    TESetText(buffer, strlen(buffer), te);
    TESetSelect(0, 32767, te);  /* Select all */
    TEActivate(te);

    /* Button areas */
    SetRect(&ok_rect, 210, 35, 280, 55);
    SetRect(&cancel_rect, 210, 65, 280, 85);

    /* Draw buttons */
    PenSize(2, 2);
    FrameRect(&ok_rect);
    PenSize(1, 1);
    MoveTo(233, 50);
    DrawString("\pOK");

    FrameRect(&cancel_rect);
    MoveTo(220, 80);
    DrawString("\pCancel");

    /* Draw hint text */
    TextSize(9);
    MoveTo(20, 75);
    DrawString("\p(e.g., 1024 for 1MB, 4096 for 4MB)");
    MoveTo(20, 90);
    DrawString("\pChanges take effect after restart.");
    TextSize(12);

    /* Event loop */
    while (!done) {
        TEIdle(te);

        if (WaitNextEvent(everyEvent, &event, 10, NULL)) {
            switch (event.what) {
                case mouseDown:
                    mouse = event.where;
                    GlobalToLocal(&mouse);

                    if (PtInRect(mouse, &ok_rect)) {
                        /* OK clicked - get value */
                        int len = (*te)->teLength;
                        if (len > 0 && len < 30) {
                            char val_str[32];
                            BlockMove(*((*te)->hText), val_str, len);
                            val_str[len] = '\0';
                            long new_val = atol(val_str);
                            if (new_val >= 64 && new_val <= 65536) {
                                g_cache_size_kb = new_val;
                                accepted = 1;
                            }
                        }
                        done = 1;
                    } else if (PtInRect(mouse, &cancel_rect)) {
                        done = 1;
                    } else if (PtInRect(mouse, &input_rect)) {
                        TEClick(mouse, (event.modifiers & shiftKey) != 0, te);
                    }
                    break;

                case keyDown:
                case autoKey: {
                    char key = event.message & charCodeMask;
                    if (key == 0x0D || key == 0x03) {  /* Return or Enter */
                        int len = (*te)->teLength;
                        if (len > 0 && len < 30) {
                            char val_str[32];
                            BlockMove(*((*te)->hText), val_str, len);
                            val_str[len] = '\0';
                            long new_val = atol(val_str);
                            if (new_val >= 64 && new_val <= 65536) {
                                g_cache_size_kb = new_val;
                                accepted = 1;
                            }
                        }
                        done = 1;
                    } else if (key == 0x1B) {  /* Escape */
                        done = 1;
                    } else {
                        TEKey(key, te);
                    }
                    break;
                }

                case updateEvt:
                    BeginUpdate(dialog);
                    /* Redraw */
                    MoveTo(20, 25);
                    DrawString("\pEnter cache size in KB:");
                    SetRect(&input_rect, 20, 35, 200, 55);
                    FrameRect(&input_rect);
                    PenSize(2, 2);
                    SetRect(&ok_rect, 210, 35, 280, 55);
                    FrameRect(&ok_rect);
                    PenSize(1, 1);
                    MoveTo(233, 50);
                    DrawString("\pOK");
                    SetRect(&cancel_rect, 210, 65, 280, 85);
                    FrameRect(&cancel_rect);
                    MoveTo(220, 80);
                    DrawString("\pCancel");
                    TextSize(9);
                    MoveTo(20, 75);
                    DrawString("\p(e.g., 1024 for 1MB, 4096 for 4MB)");
                    MoveTo(20, 90);
                    DrawString("\pChanges take effect after restart.");
                    TextSize(12);
                    InsetRect(&input_rect, 3, 3);
                    TEUpdate(&input_rect, te);
                    EndUpdate(dialog);
                    break;
            }
        }
    }

    TEDispose(te);
    DisposeWindow(dialog);

    /* Restore port to console window */
    if (g_console_window != NULL) {
        SetPort(g_console_window);
    }

    if (accepted) {
        console_printf("Cache size set to %ld KB\r", g_cache_size_kb);
        console_print("(Will take effect after restart)\r");
    }
}

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

    /* Create Server menu */
    g_server_menu = NewMenu(MENU_SERVER, "\pServer");
    AppendMenu(g_server_menu, "\pView Distance: 1/1");
    AppendMenu(g_server_menu, "\pView Distance: 2/2");
    AppendMenu(g_server_menu, "\pView Distance: 3/3");
    AppendMenu(g_server_menu, "\pView Distance: 4/4");
    AppendMenu(g_server_menu, "\p(-");  /* Separator */
    AppendMenu(g_server_menu, "\pSet Cache Size...");
    AppendMenu(g_server_menu, "\p(-");  /* Separator */
    AppendMenu(g_server_menu, "\pSmooth Mob Movement");
    InsertMenu(g_server_menu, 0);
    update_view_distance_checkmarks();
    CheckItem(g_server_menu, ITEM_SERVER_INTERP, g_mob_interpolation);

    /* Create Debug menu */
    g_debug_menu = NewMenu(MENU_DEBUG, "\pDebug");
    AppendMenu(g_debug_menu, "\pEnable Profiling/P");
    AppendMenu(g_debug_menu, "\pSave Report/R");
    AppendMenu(g_debug_menu, "\pReset Stats");
    AppendMenu(g_debug_menu, "\p(-");  /* Separator */
    AppendMenu(g_debug_menu, "\pUse Open Transport");
    AppendMenu(g_debug_menu, "\pUse MacTCP");
    AppendMenu(g_debug_menu, "\p(-");  /* Separator */
    AppendMenu(g_debug_menu, "\pRestart Server");
    /* Disable OT option if not available */
    if (!net_is_open_transport_available()) {
        DisableItem(g_debug_menu, ITEM_DEBUG_USE_OT);
    }
    InsertMenu(g_debug_menu, 0);
    update_net_stack_checkmarks();

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

        case MENU_SERVER:
            if (item_id >= ITEM_SERVER_VD1 && item_id <= ITEM_SERVER_VD4) {
                view_distance = item_id;  /* VD1=1, VD2=2, etc. */
                update_view_distance_checkmarks();
                console_printf("View distance set to %d\r", view_distance);
            } else if (item_id == ITEM_SERVER_CACHE) {
                show_cache_size_dialog();
            } else if (item_id == ITEM_SERVER_INTERP) {
                g_mob_interpolation = !g_mob_interpolation;
                CheckItem(g_server_menu, ITEM_SERVER_INTERP, g_mob_interpolation);
                console_printf("Mob interpolation %s\r", g_mob_interpolation ? "enabled" : "disabled");
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
                case ITEM_DEBUG_USE_OT:
                    if (net_set_stack(1) == 0) {
                        update_net_stack_checkmarks();
                    }
                    break;
                case ITEM_DEBUG_USE_TCP:
                    if (net_set_stack(0) == 0) {
                        update_net_stack_checkmarks();
                    }
                    break;
                case ITEM_DEBUG_RESTART:
                    net_shutdown();
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

int console_get_cache_size(void) {
    /* Convert KB to number of entries */
    /* Each CachedChunkSection is approximately 4KB */
    return (int)(g_cache_size_kb / 4);
}

int console_get_mob_interpolation(void) {
    return g_mob_interpolation;
}

/* Preferences file handling */
#define PREFS_FILE_NAME "\pBareiron Prefs"
#define PREFS_CREATOR 'BARI'
#define PREFS_TYPE 'pref'

/* Preferences structure */
typedef struct {
    long magic;           /* 'BARI' to identify valid prefs */
    short version;        /* Prefs format version */
    short view_dist;      /* View distance setting */
    long cache_size_kb;   /* Cache size in KB */
    short mob_interp;     /* Mob interpolation enabled */
} BareironPrefs;

#define PREFS_MAGIC 'BARI'
#define PREFS_VERSION 2

void console_save_prefs(void) {
    OSErr err;
    short vRefNum;
    long dirID;
    FSSpec spec;
    short refNum;
    long count;
    BareironPrefs prefs;

    /* Find the Preferences folder */
    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kCreateFolder, &vRefNum, &dirID);
    if (err != noErr) {
        console_print("Could not find Preferences folder\r");
        return;
    }

    /* Create FSSpec for our prefs file */
    err = FSMakeFSSpec(vRefNum, dirID, PREFS_FILE_NAME, &spec);

    /* Create the file if it doesn't exist */
    if (err == fnfErr) {
        err = FSpCreate(&spec, PREFS_CREATOR, PREFS_TYPE, smSystemScript);
        if (err != noErr) {
            console_print("Could not create prefs file\r");
            return;
        }
    } else if (err != noErr) {
        console_print("Error accessing prefs file\r");
        return;
    }

    /* Open the file for writing */
    err = FSpOpenDF(&spec, fsWrPerm, &refNum);
    if (err != noErr) {
        console_print("Could not open prefs file for writing\r");
        return;
    }

    /* Fill in prefs structure */
    prefs.magic = PREFS_MAGIC;
    prefs.version = PREFS_VERSION;
    prefs.view_dist = view_distance;
    prefs.cache_size_kb = g_cache_size_kb;
    prefs.mob_interp = g_mob_interpolation;

    /* Write prefs */
    count = sizeof(BareironPrefs);
    err = FSWrite(refNum, &count, &prefs);
    if (err != noErr) {
        console_print("Error writing prefs\r");
    } else {
        console_printf("Saved prefs: view_dist=%d, cache=%ldKB, interp=%d\r",
                       view_distance, g_cache_size_kb, g_mob_interpolation);
    }

    FSClose(refNum);
}

void console_load_prefs(void) {
    OSErr err;
    short vRefNum;
    long dirID;
    FSSpec spec;
    short refNum;
    long count;
    BareironPrefs prefs;

    /* Find the Preferences folder */
    err = FindFolder(kOnSystemDisk, kPreferencesFolderType, kDontCreateFolder, &vRefNum, &dirID);
    if (err != noErr) {
        return;  /* No prefs folder, use defaults */
    }

    /* Create FSSpec for our prefs file */
    err = FSMakeFSSpec(vRefNum, dirID, PREFS_FILE_NAME, &spec);
    if (err != noErr) {
        return;  /* No prefs file, use defaults */
    }

    /* Open the file for reading */
    err = FSpOpenDF(&spec, fsRdPerm, &refNum);
    if (err != noErr) {
        return;  /* Can't open, use defaults */
    }

    /* Read prefs */
    count = sizeof(BareironPrefs);
    err = FSRead(refNum, &count, &prefs);
    FSClose(refNum);

    if (err != noErr) {
        return;  /* Read failed, use defaults */
    }

    /* Validate magic number */
    if (prefs.magic != PREFS_MAGIC) {
        return;  /* Invalid prefs file */
    }

    /* Apply settings */
    if (prefs.view_dist >= 1 && prefs.view_dist <= 4) {
        view_distance = prefs.view_dist;
    }
    if (prefs.cache_size_kb >= 64 && prefs.cache_size_kb <= 65536) {
        g_cache_size_kb = prefs.cache_size_kb;
    }
    /* Only load mob_interp if version >= 2 */
    if (prefs.version >= 2) {
        g_mob_interpolation = prefs.mob_interp ? 1 : 0;
    }

    console_printf("Loaded prefs: view_dist=%d, cache=%ldKB, interp=%d\r",
                   view_distance, g_cache_size_kb, g_mob_interpolation);
}

#endif /* MAC68K_PLATFORM */
