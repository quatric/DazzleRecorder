/*
 * Dazzle Capture for Windows: live preview and MPEG-2 recording, plain
 * Win32 so it runs on any Windows 8.1+ PC (x64, x86, ARM64) without a runtime.
 *
 * The control bar under the preview selects the input, deinterlacing,
 * recording quality and folder, and starts/stops recording to a
 * DVD-compatible MPEG-2 program stream (.mpg).
 */
#define COBJMACROS
#ifndef UNICODE
#define UNICODE
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app_core.h"

enum {
    ID_INPUT = 100, ID_DEINT, ID_WIDE, ID_QUALITY, ID_FOLDER, ID_RECORD, ID_MONITOR, ID_TIME, ID_STATUS, ID_METER_L,
    ID_METER_R, ID_LBL_INPUT, ID_LBL_QUALITY, ID_LBL_SAVE, ID_LBL_L, ID_LBL_R,
    IDM_RECORD = 200, IDM_SNAPSHOT, IDM_CHOOSE_FOLDER, IDM_OPEN_FOLDER, IDM_EXIT, IDM_FULLSCREEN, IDM_ESCAPE,
    TIMER_FRAME = 1, TIMER_UI = 2,
    WM_APP_RECDONE = WM_APP + 1,
};

static struct {
    HINSTANCE inst;
    HWND wnd, preview;
    HWND input, deint, wide, quality, folder_btn, record, monitor, time, status, meter[2];
    HWND labels[5];
    HFONT font, time_font, badge_font;
    HBRUSH bar_brush;
    HMENU menu;
    int dpi;
    app_t *app;
    uint32_t *frame;
    uint64_t seq;
    int w, h;
    char folder[1024];
    int finishing;
    double level[2];
    int fullscreen;
    WINDOWPLACEMENT placement;
} G;

static int S(int v) { return MulDiv(v, G.dpi, 96); }

static void utf8_to_w(const char *s, wchar_t *w, int len)
{
    if (!MultiByteToWideChar(CP_UTF8, 0, s, -1, w, len))
        w[0] = 0;
}

static void w_to_utf8(const wchar_t *w, char *s, int len)
{
    if (!WideCharToMultiByte(CP_UTF8, 0, w, -1, s, len, NULL, NULL))
        s[0] = 0;
}

static void set_text(HWND h, const char *utf8)
{
    wchar_t w[1200];
    utf8_to_w(utf8, w, 1200);
    SetWindowTextW(h, w);
}

static void set_status(const char *s) { set_text(G.status, s ? s : ""); }

static const char *base_name(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++)
        if (*p == '\\' || *p == '/')
            b = p + 1;
    return b;
}

static void update_title(void)
{
    char t[160];
    int sv = (int)SendMessageW(G.input, CB_GETCURSEL, 0, 0) == 1;
    snprintf(t, sizeof(t), APP_NAME " \xe2\x80\x94 %s %s", sv ? "S-Video" : "Composite",
             app_is_pal(G.app) ? "PAL 720\xc3\x97" "576" : "NTSC 720\xc3\x97" "480");
    set_text(G.wnd, t);
}

static void update_folder_button(void)
{
    char label[300];
    snprintf(label, sizeof(label), "\xf0\x9f\x93\x81 %s", base_name(G.folder)[0] ? base_name(G.folder) : G.folder);
    set_text(G.folder_btn, label);
}

/* ---- preview ---- */

static int app_widescreen_on; /* mirrors the Widescreen checkbox for the paint code */

static RECT picture_rect(RECT c)
{
    int cw = c.right - c.left, ch = c.bottom - c.top;
    int wide = G.app && app_widescreen_on;
    int w = cw, h = wide ? cw * 9 / 16 : cw * 3 / 4;
    if (h > ch) {
        h = ch;
        w = wide ? ch * 16 / 9 : ch * 4 / 3;
    }
    RECT r = { c.left + (cw - w) / 2, c.top + (ch - h) / 2, 0, 0 };
    r.right = r.left + w;
    r.bottom = r.top + h;
    return r;
}

static LRESULT CALLBACK preview_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_LBUTTONDBLCLK:
        SendMessageW(G.wnd, WM_COMMAND, IDM_FULLSCREEN, 0);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c, r;
        GetClientRect(h, &c);
        r = picture_rect(c);
        /* black bars around the 4:3 picture */
        HBRUSH black = GetStockObject(BLACK_BRUSH);
        RECT bars[4] = { { c.left, c.top, c.right, r.top }, { c.left, r.bottom, c.right, c.bottom },
                         { c.left, r.top, r.left, r.bottom }, { r.right, r.top, c.right, r.bottom } };
        for (int i = 0; i < 4; i++)
            FillRect(dc, &bars[i], black);
        /* 720 BT.601 samples span a 4:3 picture, so stretch into the 4:3 rect */
        BITMAPINFO bi = { 0 };
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = G.w;
        bi.bmiHeader.biHeight = -G.h; /* top-down */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        SetStretchBltMode(dc, HALFTONE);
        SetBrushOrgEx(dc, 0, 0, NULL);
        StretchDIBits(dc, r.left, r.top, r.right - r.left, r.bottom - r.top, 0, 0, G.w, G.h, G.frame, &bi,
                      DIB_RGB_COLORS, SRCCOPY);
        if (G.app && app_is_recording(G.app)) {
            wchar_t badge[64];
            int s = (int)app_record_seconds(G.app);
            swprintf(badge, 64, L" \x25CF REC %02d:%02d:%02d ", s / 3600, s / 60 % 60, s % 60);
            HFONT old = SelectObject(dc, G.badge_font);
            RECT tr = { 0, 0, 0, 0 };
            DrawTextW(dc, badge, -1, &tr, DT_CALCRECT | DT_SINGLELINE);
            RECT br = { r.left + S(10), r.top + S(10), r.left + S(10) + tr.right + S(8), r.top + S(10) + S(22) };
            HBRUSH red = CreateSolidBrush(RGB(204, 13, 13));
            FillRect(dc, &br, red);
            DeleteObject(red);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, RGB(255, 255, 255));
            DrawTextW(dc, badge, -1, &br, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            SelectObject(dc, old);
        }
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

/* ---- meters (owner drawn statics) ---- */

static void draw_meter(DRAWITEMSTRUCT *d)
{
    int c = d->CtlID == ID_METER_R;
    RECT r = d->rcItem;
    HBRUSH bg = CreateSolidBrush(RGB(60, 60, 60));
    FillRect(d->hDC, &r, bg);
    DeleteObject(bg);
    double v = G.level[c] / 60.0; /* dBFS + 60 */
    int w = (int)((r.right - r.left) * v);
    COLORREF col = G.level[c] > 57 ? RGB(230, 50, 40) : G.level[c] > 48 ? RGB(240, 200, 40) : RGB(60, 200, 80);
    RECT f = { r.left, r.top, r.left + w, r.bottom };
    HBRUSH b = CreateSolidBrush(col);
    FillRect(d->hDC, &f, b);
    DeleteObject(b);
}

/* ---- layout ---- */

#define BAR_H 84

static void layout(void)
{
    RECT c;
    GetClientRect(G.wnd, &c);
    if (G.fullscreen) {
        MoveWindow(G.preview, 0, 0, c.right, c.bottom, TRUE);
        return;
    }
    int bar = S(BAR_H), y1 = c.bottom - bar + S(10), y2 = c.bottom - bar + S(46);
    MoveWindow(G.preview, 0, 0, c.right, c.bottom - bar, TRUE);
    int x = S(12), lh = S(20), ch = S(24);
    struct { HWND h; int w, dy, hh; } row1[] = {
        { G.labels[0], 40, 3, 20 }, { G.input, 110, 0, 200 }, { G.deint, 100, 2, 20 }, { G.wide, 100, 2, 20 },
        { G.labels[1], 52, 3, 20 }, { G.quality, 170, 0, 200 }, { G.labels[2], 56, 3, 20 },
        { G.folder_btn, 220, -1, 26 },
    };
    for (size_t i = 0; i < sizeof(row1) / sizeof(row1[0]); i++) {
        MoveWindow(row1[i].h, x, y1 + S(row1[i].dy), S(row1[i].w), S(row1[i].hh), TRUE);
        x += S(row1[i].w) + S(i == 1 || i == 3 || i == 5 ? 16 : 6);
    }
    x = S(12);
    MoveWindow(G.record, x, y2, S(110), S(28), TRUE);
    x += S(110 + 16);
    MoveWindow(G.time, x, y2 + S(3), S(84), lh + S(2), TRUE);
    x += S(84 + 12);
    MoveWindow(G.labels[3], x, y2, S(12), S(14), TRUE);
    MoveWindow(G.meter[0], x + S(14), y2 + S(4), S(100), S(8), TRUE);
    MoveWindow(G.labels[4], x, y2 + S(14), S(12), S(14), TRUE);
    MoveWindow(G.meter[1], x + S(14), y2 + S(18), S(100), S(8), TRUE);
    x += S(14 + 100 + 16);
    MoveWindow(G.monitor, x, y2 + S(4), S(100), lh, TRUE);
    x += S(100 + 12);
    MoveWindow(G.status, x, y2 + S(6), c.right - x - S(12), lh, TRUE);
    (void)ch;
}

static HWND make(const wchar_t *cls, const wchar_t *text, DWORD style, int id)
{
    HWND h = CreateWindowExW(0, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, G.wnd,
                             (HMENU)(INT_PTR)id, G.inst, NULL);
    SendMessageW(h, WM_SETFONT, (WPARAM)G.font, TRUE);
    return h;
}

static void build_controls(void)
{
    G.preview = CreateWindowExW(0, L"DazzlePreview", L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, G.wnd, NULL,
                                G.inst, NULL);
    G.labels[0] = make(L"STATIC", L"Input", 0, ID_LBL_INPUT);
    G.input = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, ID_INPUT);
    SendMessageW(G.input, CB_ADDSTRING, 0, (LPARAM)L"Composite");
    SendMessageW(G.input, CB_ADDSTRING, 0, (LPARAM)L"S-Video");
    G.deint = make(L"BUTTON", L"Deinterlace", BS_AUTOCHECKBOX | WS_TABSTOP, ID_DEINT);
    G.wide = make(L"BUTTON", L"Widescreen", BS_AUTOCHECKBOX | WS_TABSTOP, ID_WIDE);
    G.labels[1] = make(L"STATIC", L"Quality", 0, ID_LBL_QUALITY);
    G.quality = make(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_TABSTOP, ID_QUALITY);
    SendMessageW(G.quality, CB_ADDSTRING, 0, (LPARAM)L"Standard (6 Mbit/s)");
    SendMessageW(G.quality, CB_ADDSTRING, 0, (LPARAM)L"High (8 Mbit/s)");
    SendMessageW(G.quality, CB_ADDSTRING, 0, (LPARAM)L"Maximum (9.5 Mbit/s)");
    G.labels[2] = make(L"STATIC", L"Save to", 0, ID_LBL_SAVE);
    G.folder_btn = make(L"BUTTON", L"", BS_PUSHBUTTON | WS_TABSTOP, ID_FOLDER);

    G.record = make(L"BUTTON", L"\x25CF  Record", BS_PUSHBUTTON | WS_TABSTOP, ID_RECORD);
    G.time = make(L"STATIC", L"00:00:00", 0, ID_TIME);
    SendMessageW(G.time, WM_SETFONT, (WPARAM)G.time_font, TRUE);
    G.labels[3] = make(L"STATIC", L"L", 0, ID_LBL_L);
    G.meter[0] = make(L"STATIC", L"", SS_OWNERDRAW, ID_METER_L);
    G.labels[4] = make(L"STATIC", L"R", 0, ID_LBL_R);
    G.meter[1] = make(L"STATIC", L"", SS_OWNERDRAW, ID_METER_R);
    G.monitor = make(L"BUTTON", L"Play audio", BS_AUTOCHECKBOX | WS_TABSTOP, ID_MONITOR);
    G.status = make(L"STATIC", L"", SS_ENDELLIPSIS | SS_NOPREFIX, ID_STATUS);
}

static void build_menu(void)
{
    HMENU file = CreatePopupMenu(), view = CreatePopupMenu();
    AppendMenuW(file, MF_STRING, IDM_RECORD, L"&Start Recording\tCtrl+R");
    AppendMenuW(file, MF_STRING, IDM_SNAPSHOT, L"Save &Snapshot\tCtrl+S");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_CHOOSE_FOLDER, L"Choose Recording &Folder\x2026");
    AppendMenuW(file, MF_STRING, IDM_OPEN_FOLDER, L"&Open Recordings Folder");
    AppendMenuW(file, MF_SEPARATOR, 0, NULL);
    AppendMenuW(file, MF_STRING, IDM_EXIT, L"E&xit");
    AppendMenuW(view, MF_STRING, IDM_FULLSCREEN, L"&Full Screen\tF11");
    G.menu = CreateMenu();
    AppendMenuW(G.menu, MF_POPUP, (UINT_PTR)file, L"&File");
    AppendMenuW(G.menu, MF_POPUP, (UINT_PTR)view, L"&View");
    SetMenu(G.wnd, G.menu);
}

/* ---- actions ---- */

static void choose_folder(void)
{
    static const GUID clsid = { 0xdc1c5a9c, 0xe88a, 0x4dde, { 0xa5, 0xa1, 0x60, 0xf8, 0x2a, 0x20, 0xae, 0xf7 } };
    static const GUID iid = { 0xd57c7288, 0xd4ad, 0x4768, { 0xbe, 0x02, 0x9d, 0x96, 0x95, 0x32, 0xd9, 0x60 } };
    static const GUID iid_item = { 0x43826d1e, 0xe718, 0x42ee, { 0xbc, 0x55, 0xa1, 0xe2, 0x61, 0xc3, 0x7b, 0xfe } };
    IFileOpenDialog *dlg = NULL;
    if (FAILED(CoCreateInstance(&clsid, NULL, CLSCTX_INPROC_SERVER, &iid, (void **)&dlg)))
        return;
    DWORD opts = 0;
    IFileOpenDialog_GetOptions(dlg, &opts);
    IFileOpenDialog_SetOptions(dlg, opts | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    IFileOpenDialog_SetTitle(dlg, L"Choose the recording folder");
    wchar_t wf[1024];
    IShellItem *start = NULL;
    utf8_to_w(G.folder, wf, 1024);
    if (SUCCEEDED(SHCreateItemFromParsingName(wf, NULL, &iid_item, (void **)&start))) {
        IFileOpenDialog_SetFolder(dlg, start);
        IShellItem_Release(start);
    }
    if (SUCCEEDED(IFileOpenDialog_Show(dlg, G.wnd))) {
        IShellItem *item = NULL;
        if (SUCCEEDED(IFileOpenDialog_GetResult(dlg, &item))) {
            PWSTR p = NULL;
            if (SUCCEEDED(IShellItem_GetDisplayName(item, SIGDN_FILESYSPATH, &p))) {
                w_to_utf8(p, G.folder, sizeof(G.folder));
                CoTaskMemFree(p);
                app_set_setting_str("folder", G.folder);
                update_folder_button();
            }
            IShellItem_Release(item);
        }
    }
    IFileOpenDialog_Release(dlg);
}

static void open_folder(void)
{
    wchar_t w[1024];
    utf8_to_w(G.folder, w, 1024);
    ShellExecuteW(G.wnd, L"open", w, NULL, NULL, SW_SHOWNORMAL);
}

static void reveal_file(const char *path)
{
    wchar_t w[1024];
    utf8_to_w(path, w, 1024);
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(w);
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
        ILFree(pidl);
    }
}

static void set_recording_ui(int recording)
{
    SetWindowTextW(G.record, recording ? L"\x25A0  Stop" : L"\x25CF  Record");
    EnableWindow(G.deint, !recording);
    EnableWindow(G.wide, !recording);
    EnableWindow(G.quality, !recording);
    EnableWindow(G.folder_btn, !recording);
    ModifyMenuW(G.menu, IDM_RECORD, MF_BYCOMMAND | MF_STRING, IDM_RECORD,
                recording ? L"&Stop Recording\tCtrl+R" : L"&Start Recording\tCtrl+R");
}

static void rec_done(void *user, int ok, const char *path)
{
    (void)user;
    size_t n = strlen(path) + 1;
    char *copy = malloc(n);
    if (copy)
        memcpy(copy, path, n);
    PostMessageW(G.wnd, WM_APP_RECDONE, (WPARAM)ok, (LPARAM)copy);
}

static void toggle_record(void)
{
    if (G.finishing)
        return;
    if (app_is_recording(G.app)) {
        G.finishing = 1;
        EnableWindow(G.record, FALSE);
        SetWindowTextW(G.record, L"Finishing\x2026");
        set_status("Finishing the file\xe2\x80\xa6");
        app_record_stop(G.app, rec_done, NULL);
        return;
    }
    char path[1100], err[300], msg[1300];
    int deint = SendMessageW(G.deint, BM_GETCHECK, 0, 0) == BST_CHECKED;
    int q = (int)SendMessageW(G.quality, CB_GETCURSEL, 0, 0);
    if (app_record_start(G.app, G.folder, q, deint, path, sizeof(path), err, sizeof(err))) {
        wchar_t w[300];
        utf8_to_w(err, w, 300);
        MessageBoxW(G.wnd, w, L"Cannot start recording", MB_ICONWARNING);
        return;
    }
    set_recording_ui(1);
    snprintf(msg, sizeof(msg), "Recording %s MPEG-2 to %s", deint ? "deinterlaced" : "interlaced", base_name(path));
    set_status(msg);
}

static void snapshot(void)
{
    char path[1100], err[300], msg[1300];
    if (app_snapshot(G.app, G.folder, path, sizeof(path), err, sizeof(err)))
        set_status(err);
    else {
        snprintf(msg, sizeof(msg), "Saved %s", base_name(path));
        set_status(msg);
    }
}

static void toggle_fullscreen(void)
{
    DWORD style = (DWORD)GetWindowLongW(G.wnd, GWL_STYLE);
    HWND bar[] = { G.input, G.deint, G.wide, G.quality, G.folder_btn, G.record, G.monitor, G.time, G.status,
                   G.meter[0], G.meter[1], G.labels[0], G.labels[1], G.labels[2], G.labels[3], G.labels[4] };
    G.fullscreen = !G.fullscreen;
    for (size_t i = 0; i < sizeof(bar) / sizeof(bar[0]); i++)
        ShowWindow(bar[i], G.fullscreen ? SW_HIDE : SW_SHOW);
    if (G.fullscreen) {
        MONITORINFO mi = { sizeof(mi) };
        G.placement.length = sizeof(G.placement);
        GetWindowPlacement(G.wnd, &G.placement);
        GetMonitorInfoW(MonitorFromWindow(G.wnd, MONITOR_DEFAULTTONEAREST), &mi);
        SetMenu(G.wnd, NULL);
        SetWindowLongW(G.wnd, GWL_STYLE, (LONG)(style & ~WS_OVERLAPPEDWINDOW));
        SetWindowPos(G.wnd, HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top, mi.rcMonitor.right - mi.rcMonitor.left,
                     mi.rcMonitor.bottom - mi.rcMonitor.top, SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    } else {
        SetWindowLongW(G.wnd, GWL_STYLE, (LONG)(style | WS_OVERLAPPEDWINDOW));
        SetMenu(G.wnd, G.menu);
        SetWindowPlacement(G.wnd, &G.placement);
        SetWindowPos(G.wnd, NULL, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
    }
    layout();
}

static void tick_ui(void)
{
    double db[2];
    app_peaks(G.app, db);
    for (int c = 0; c < 2; c++) {
        double v = db[c] + 60, decayed = G.level[c] - 1.5;
        G.level[c] = v > decayed ? v : decayed < 0 ? 0 : decayed;
        InvalidateRect(G.meter[c], NULL, FALSE);
    }
    if (app_is_recording(G.app)) {
        int s = (int)app_record_seconds(G.app);
        char t[32];
        snprintf(t, sizeof(t), "%02d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
        set_text(G.time, t);
    } else if (!G.finishing) {
        set_text(G.time, "00:00:00");
    }
}

/* ---- main window ---- */

static LRESULT CALLBACK wnd_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
    case WM_SIZE:
        layout();
        return 0;
    case WM_GETMINMAXINFO: {
        MINMAXINFO *mm = (MINMAXINFO *)(void *)lp;
        mm->ptMinTrackSize.x = S(900);
        mm->ptMinTrackSize.y = S(420);
        return 0;
    }
    case WM_CTLCOLORSTATIC:
        if ((HWND)lp == G.status || (HWND)lp == G.labels[0] || (HWND)lp == G.labels[1] ||
            (HWND)lp == G.labels[2] || (HWND)lp == G.labels[3] || (HWND)lp == G.labels[4])
            SetTextColor((HDC)wp, GetSysColor(COLOR_GRAYTEXT));
        SetBkColor((HDC)wp, GetSysColor(COLOR_BTNFACE));
        return (LRESULT)G.bar_brush;
    case WM_DRAWITEM:
        draw_meter((DRAWITEMSTRUCT *)(void *)lp);
        return TRUE;
    case WM_TIMER:
        if (wp == TIMER_FRAME) {
            if (app_get_frame(G.app, G.frame, &G.seq))
                InvalidateRect(G.preview, NULL, FALSE);
        } else {
            tick_ui();
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_INPUT:
            if (HIWORD(wp) == CBN_SELCHANGE) {
                int i = (int)SendMessageW(G.input, CB_GETCURSEL, 0, 0);
                app_set_input(G.app, i == 1 ? DVC100_INPUT_SVIDEO : DVC100_INPUT_COMPOSITE);
                app_set_setting_int("input", i);
                update_title();
            }
            return 0;
        case ID_QUALITY:
            if (HIWORD(wp) == CBN_SELCHANGE)
                app_set_setting_int("quality", (int)SendMessageW(G.quality, CB_GETCURSEL, 0, 0));
            return 0;
        case ID_DEINT: {
            int on = SendMessageW(G.deint, BM_GETCHECK, 0, 0) == BST_CHECKED;
            app_set_deinterlace(G.app, on);
            app_set_setting_int("deinterlace", on);
            return 0;
        }
        case ID_WIDE: {
            int on = SendMessageW(G.wide, BM_GETCHECK, 0, 0) == BST_CHECKED;
            app_widescreen_on = on;
            app_set_widescreen(G.app, on);
            app_set_setting_int("widescreen", on);
            InvalidateRect(G.preview, NULL, TRUE);
            return 0;
        }
        case ID_MONITOR: {
            int on = SendMessageW(G.monitor, BM_GETCHECK, 0, 0) == BST_CHECKED;
            app_set_monitor(G.app, on);
            app_set_setting_int("monitor", on);
            return 0;
        }
        case ID_FOLDER:
        case IDM_CHOOSE_FOLDER:
            choose_folder();
            return 0;
        case ID_RECORD:
        case IDM_RECORD:
            toggle_record();
            return 0;
        case IDM_SNAPSHOT:
            snapshot();
            return 0;
        case IDM_OPEN_FOLDER:
            open_folder();
            return 0;
        case IDM_FULLSCREEN:
            toggle_fullscreen();
            return 0;
        case IDM_ESCAPE:
            if (G.fullscreen)
                toggle_fullscreen();
            return 0;
        case IDM_EXIT:
            PostMessageW(h, WM_CLOSE, 0, 0);
            return 0;
        }
        break;
    case WM_APP_RECDONE: {
        char *path = (char *)lp, msg2[1300];
        G.finishing = 0;
        EnableWindow(G.record, TRUE);
        set_recording_ui(0);
        if (wp && path) {
            snprintf(msg2, sizeof(msg2), "Saved %s", base_name(path));
            set_status(msg2);
            reveal_file(path);
        } else {
            set_status("Recording failed (ffmpeg error)");
        }
        free(path);
        InvalidateRect(G.preview, NULL, FALSE);
        return 0;
    }
    case WM_CLOSE:
        if (app_is_recording(G.app) &&
            MessageBoxW(h, L"Stop the recording and quit?", L"A recording is in progress",
                        MB_ICONQUESTION | MB_OKCANCEL) != IDOK)
            return 0;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
    (void)prev, (void)cmdline;
    G.inst = inst;
    SetProcessDPIAware();
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    HDC sdc = GetDC(NULL);
    G.dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(NULL, sdc);

    dvc100_input_t input = app_setting_int("input", 0) == 1 ? DVC100_INPUT_SVIDEO : DVC100_INPUT_COMPOSITE;
    dvc100_norm_t norm = DVC100_NORM_AUTO;
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i < argc; i++) {
        if (!wcscmp(argv[i], L"--svideo")) input = DVC100_INPUT_SVIDEO;
        if (!wcscmp(argv[i], L"--composite")) input = DVC100_INPUT_COMPOSITE;
        if (!wcscmp(argv[i], L"--pal")) norm = DVC100_NORM_PAL;
        if (!wcscmp(argv[i], L"--ntsc")) norm = DVC100_NORM_NTSC;
        if (!wcscmp(argv[i], L"-v")) dvc100_set_verbose(1);
    }
    LocalFree(argv);

    char err[600];
    G.app = app_open(input, norm, err, sizeof(err));
    if (!G.app) {
        wchar_t w[600];
        utf8_to_w(err, w, 600);
        MessageBoxW(NULL, w, L"No Dazzle capture device available", MB_ICONERROR);
        return 1;
    }
    G.w = app_width(G.app);
    G.h = app_height(G.app);
    G.frame = calloc((size_t)G.w * G.h, 4);

    char def[1024];
    app_default_folder(def, sizeof(def));
    app_setting_str("folder", G.folder, sizeof(G.folder), def);

    NONCLIENTMETRICSW ncm = { sizeof(ncm) };
    SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
    G.font = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfHeight = -S(17);
    lf.lfWeight = FW_MEDIUM;
    wcscpy(lf.lfFaceName, L"Consolas");
    G.time_font = CreateFontIndirectW(&lf);
    lf = ncm.lfMessageFont;
    lf.lfHeight = -S(14);
    lf.lfWeight = FW_SEMIBOLD;
    G.badge_font = CreateFontIndirectW(&lf);
    G.bar_brush = GetSysColorBrush(COLOR_BTNFACE);

    WNDCLASSEXW wc = { sizeof(wc) };
    wc.lpfnWndProc = preview_proc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = CS_DBLCLKS;
    wc.lpszClassName = L"DazzlePreview";
    RegisterClassExW(&wc);

    wc.lpfnWndProc = wnd_proc;
    wc.style = 0;
    wc.hbrBackground = G.bar_brush;
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.hIconSm = wc.hIcon;
    wc.lpszClassName = L"DazzleCapture";
    RegisterClassExW(&wc);

    RECT r = { 0, 0, S(960), S(600 + BAR_H) };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, TRUE);
    G.wnd = CreateWindowExW(0, L"DazzleCapture", L"Dazzle Capture", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                            CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top, NULL, NULL, inst,
                            NULL);
    build_menu();
    build_controls();

    SendMessageW(G.input, CB_SETCURSEL, input == DVC100_INPUT_SVIDEO ? 1 : 0, 0);
    int deint = app_setting_int("deinterlace", 1), monitor = app_setting_int("monitor", 1);
    SendMessageW(G.deint, BM_SETCHECK, deint ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(G.monitor, BM_SETCHECK, monitor ? BST_CHECKED : BST_UNCHECKED, 0);
    SendMessageW(G.quality, CB_SETCURSEL, app_setting_int("quality", 1), 0);
    int wide = app_setting_int("widescreen", 0);
    SendMessageW(G.wide, BM_SETCHECK, wide ? BST_CHECKED : BST_UNCHECKED, 0);
    app_widescreen_on = wide;
    app_set_widescreen(G.app, wide);
    app_set_deinterlace(G.app, deint);
    app_set_monitor(G.app, monitor);
    update_folder_button();
    update_title();
    set_status(app_audio_problem(G.app));
    layout();

    ACCEL acc[] = { { FCONTROL | FVIRTKEY, 'R', IDM_RECORD }, { FCONTROL | FVIRTKEY, 'S', IDM_SNAPSHOT },
                    { FVIRTKEY, VK_F11, IDM_FULLSCREEN }, { FVIRTKEY, VK_ESCAPE, IDM_ESCAPE } };
    HACCEL accel = CreateAcceleratorTableW(acc, 4);

    ShowWindow(G.wnd, show);
    SetTimer(G.wnd, TIMER_FRAME, 15, NULL);
    SetTimer(G.wnd, TIMER_UI, 50, NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!TranslateAcceleratorW(G.wnd, accel, &m)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
        }
    }
    app_close(G.app); /* finishes a recording still in progress */
    CoUninitialize();
    return 0;
}
