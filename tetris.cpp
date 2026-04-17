#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objidl.h>
#include <algorithm>
using std::min;
using std::max;
#include <gdiplus.h>
#include <cstdlib>
#include <ctime>
#include <cstdio>
#include <cwchar>

using namespace Gdiplus;

// -------- Layout --------
static const int COLS       = 10;
static const int ROWS       = 20;
static const int BLOCK      = 30;
static const int BOARD_X    = 40;
static const int BOARD_Y    = 50;
static const int BOARD_W    = COLS * BLOCK;              // 300
static const int BOARD_H    = ROWS * BLOCK;              // 600
static const int PANEL_X    = BOARD_X + BOARD_W + 30;    // 370
static const int PANEL_W    = 180;
static const int CLIENT_W   = PANEL_X + PANEL_W + 30;    // 580
static const int CLIENT_H   = BOARD_Y + BOARD_H + 50;    // 700

// -------- Tetromino shapes: [type][rotation][block][x,y] --------
static const int SHAPES[7][4][4][2] = {
    // I
    {{{0,1},{1,1},{2,1},{3,1}}, {{2,0},{2,1},{2,2},{2,3}}, {{0,2},{1,2},{2,2},{3,2}}, {{1,0},{1,1},{1,2},{1,3}}},
    // O
    {{{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{2,1}}},
    // T
    {{{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}}},
    // S
    {{{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}}, {{1,1},{2,1},{0,2},{1,2}}, {{0,0},{0,1},{1,1},{1,2}}},
    // Z
    {{{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{1,2},{2,2}}, {{1,0},{0,1},{1,1},{0,2}}},
    // J
    {{{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}}, {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{0,2},{1,2}}},
    // L
    {{{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}}, {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}}},
};

struct PieceColor { Color base, light, dark; };
static PieceColor PIECE_COLORS[7];

static BYTE clampB(int v) { return v < 0 ? 0 : (v > 255 ? 255 : (BYTE)v); }
static PieceColor makePC(int r, int g, int b) {
    PieceColor pc;
    pc.base  = Color(255, (BYTE)r, (BYTE)g, (BYTE)b);
    pc.light = Color(255, clampB(r + 75), clampB(g + 75), clampB(b + 75));
    pc.dark  = Color(255, clampB((int)(r * 0.45)), clampB((int)(g * 0.45)), clampB((int)(b * 0.45)));
    return pc;
}
static void initColors() {
    PIECE_COLORS[0] = makePC(  0, 235, 255); // I cyan
    PIECE_COLORS[1] = makePC(255, 215,   0); // O yellow
    PIECE_COLORS[2] = makePC(175,  55, 235); // T purple
    PIECE_COLORS[3] = makePC( 30, 210,  85); // S green
    PIECE_COLORS[4] = makePC(235,  55,  60); // Z red
    PIECE_COLORS[5] = makePC( 45, 105, 245); // J blue
    PIECE_COLORS[6] = makePC(255, 140,  25); // L orange
}

// -------- Game state --------
static int   board[ROWS][COLS] = {0};
static int   curType = 0, curRot = 0, curX = 0, curY = 0;
static int   nextType = 0;
static int   score = 0, level = 1, linesCleared = 0;
static bool  gameOver = false;
static bool  paused   = false;
static DWORD lastFallTime = 0;
static int   fallInterval = 800;

static HWND       g_hwnd       = NULL;
static ULONG_PTR  g_gdipToken  = 0;
static HBITMAP    g_backBmp    = NULL;
static HDC        g_backDC     = NULL;
static int        g_bbW = 0, g_bbH = 0;

// -------- Game logic --------
static int randomPiece() { return rand() % 7; }

static bool canPlace(int type, int rot, int x, int y) {
    for (int i = 0; i < 4; ++i) {
        int bx = x + SHAPES[type][rot][i][0];
        int by = y + SHAPES[type][rot][i][1];
        if (bx < 0 || bx >= COLS || by >= ROWS) return false;
        if (by < 0) continue;
        if (board[by][bx] != 0) return false;
    }
    return true;
}

static void spawnPiece() {
    curType = nextType;
    nextType = randomPiece();
    curRot = 0;
    curX = 3;
    curY = 0;
    if (!canPlace(curType, curRot, curX, curY)) gameOver = true;
}

static void resetGame() {
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c)
            board[r][c] = 0;
    score = 0; level = 1; linesCleared = 0;
    gameOver = false; paused = false;
    fallInterval = 800;
    nextType = randomPiece();
    spawnPiece();
    lastFallTime = GetTickCount();
}

static void lockPiece() {
    for (int i = 0; i < 4; ++i) {
        int bx = curX + SHAPES[curType][curRot][i][0];
        int by = curY + SHAPES[curType][curRot][i][1];
        if (by >= 0 && by < ROWS && bx >= 0 && bx < COLS)
            board[by][bx] = curType + 1;
    }
    int cleared = 0;
    for (int r = ROWS - 1; r >= 0; ) {
        bool full = true;
        for (int c = 0; c < COLS; ++c) if (board[r][c] == 0) { full = false; break; }
        if (full) {
            for (int rr = r; rr > 0; --rr)
                for (int c = 0; c < COLS; ++c) board[rr][c] = board[rr - 1][c];
            for (int c = 0; c < COLS; ++c) board[0][c] = 0;
            ++cleared;
        } else --r;
    }
    if (cleared > 0) {
        static const int pts[] = {0, 100, 300, 500, 800};
        score += pts[cleared] * level;
        linesCleared += cleared;
        int newLevel = 1 + linesCleared / 10;
        if (newLevel > level) {
            level = newLevel;
            double f = 800.0;
            for (int i = 1; i < level; ++i) f *= 0.85;
            fallInterval = (int)f;
            if (fallInterval < 80) fallInterval = 80;
        }
    }
    spawnPiece();
}

static int ghostY() {
    int y = curY;
    while (canPlace(curType, curRot, curX, y + 1)) ++y;
    return y;
}

static void moveLeft()  { if (!gameOver && !paused && canPlace(curType, curRot, curX - 1, curY)) --curX; }
static void moveRight() { if (!gameOver && !paused && canPlace(curType, curRot, curX + 1, curY)) ++curX; }
static void softDrop() {
    if (gameOver || paused) return;
    if (canPlace(curType, curRot, curX, curY + 1)) { ++curY; score += 1; lastFallTime = GetTickCount(); }
}
static void rotate() {
    if (gameOver || paused) return;
    int newRot = (curRot + 1) % 4;
    const int kicks[5][2] = {{0,0},{-1,0},{1,0},{0,-1},{-2,0}};
    for (int i = 0; i < 5; ++i) {
        if (canPlace(curType, newRot, curX + kicks[i][0], curY + kicks[i][1])) {
            curRot = newRot;
            curX += kicks[i][0];
            curY += kicks[i][1];
            return;
        }
    }
}
static void hardDrop() {
    if (gameOver || paused) return;
    int drops = 0;
    while (canPlace(curType, curRot, curX, curY + 1)) { ++curY; ++drops; }
    score += drops * 2;
    lockPiece();
    lastFallTime = GetTickCount();
}

static void updateGame() {
    if (gameOver || paused) return;
    DWORD now = GetTickCount();
    if (now - lastFallTime >= (DWORD)fallInterval) {
        if (canPlace(curType, curRot, curX, curY + 1)) ++curY;
        else lockPiece();
        lastFallTime = now;
    }
}

// -------- Rendering --------
static void drawBlock(Graphics& g, int px, int py, int size, const PieceColor& pc, bool ghost = false) {
    if (ghost) {
        Pen pen(Color(170, pc.base.GetR(), pc.base.GetG(), pc.base.GetB()), 2.0f);
        g.DrawRectangle(&pen, px + 3, py + 3, size - 6, size - 6);
        SolidBrush fill(Color(35, pc.base.GetR(), pc.base.GetG(), pc.base.GetB()));
        g.FillRectangle(&fill, px + 4, py + 4, size - 8, size - 8);
        return;
    }
    Rect r(px + 1, py + 1, size - 2, size - 2);
    LinearGradientBrush grad(r, pc.light, pc.dark, LinearGradientModeForwardDiagonal);
    g.FillRectangle(&grad, r);

    SolidBrush hi(Color(110, 255, 255, 255));
    g.FillRectangle(&hi, px + 2, py + 2, size - 4, 3);
    g.FillRectangle(&hi, px + 2, py + 2, 3, size - 4);

    SolidBrush sh(Color(90, 0, 0, 0));
    g.FillRectangle(&sh, px + 2, py + size - 5, size - 4, 3);
    g.FillRectangle(&sh, px + size - 5, py + 2, 3, size - 4);

    Pen border(Color(200, 0, 0, 0), 1.0f);
    g.DrawRectangle(&border, px + 1, py + 1, size - 3, size - 3);
}

static void renderGame(HDC hdc, int w, int h) {
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.SetTextRenderingHint(TextRenderingHintClearTypeGridFit);

    // Background
    Rect full(0, 0, w, h);
    LinearGradientBrush bg(full, Color(255, 24, 28, 48), Color(255, 10, 12, 22), LinearGradientModeVertical);
    g.FillRectangle(&bg, full);

    // Title
    FontFamily fam(L"Segoe UI");
    Font titleFont(&fam, 28, FontStyleBold, UnitPixel);
    SolidBrush titleBrush(Color(255, 225, 232, 255));
    StringFormat sfc;
    sfc.SetAlignment(StringAlignmentCenter);
    g.DrawString(L"TETRIS", -1, &titleFont,
                 PointF((REAL)(BOARD_X + BOARD_W / 2), 8.0f), &sfc, &titleBrush);

    // Board background
    Rect boardR(BOARD_X, BOARD_Y, BOARD_W, BOARD_H);
    SolidBrush boardBg(Color(255, 6, 9, 22));
    g.FillRectangle(&boardBg, boardR);

    // Grid
    Pen gridPen(Color(45, 140, 160, 210), 1.0f);
    for (int c = 1; c < COLS; ++c)
        g.DrawLine(&gridPen, BOARD_X + c * BLOCK, BOARD_Y, BOARD_X + c * BLOCK, BOARD_Y + BOARD_H);
    for (int r = 1; r < ROWS; ++r)
        g.DrawLine(&gridPen, BOARD_X, BOARD_Y + r * BLOCK, BOARD_X + BOARD_W, BOARD_Y + r * BLOCK);

    // Locked blocks
    for (int r = 0; r < ROWS; ++r)
        for (int c = 0; c < COLS; ++c)
            if (board[r][c] != 0)
                drawBlock(g, BOARD_X + c * BLOCK, BOARD_Y + r * BLOCK, BLOCK, PIECE_COLORS[board[r][c] - 1]);

    // Ghost
    if (!gameOver) {
        int gy = ghostY();
        for (int i = 0; i < 4; ++i) {
            int bx = curX + SHAPES[curType][curRot][i][0];
            int by = gy   + SHAPES[curType][curRot][i][1];
            if (by >= 0) drawBlock(g, BOARD_X + bx * BLOCK, BOARD_Y + by * BLOCK, BLOCK, PIECE_COLORS[curType], true);
        }
    }

    // Current piece
    if (!gameOver) {
        for (int i = 0; i < 4; ++i) {
            int bx = curX + SHAPES[curType][curRot][i][0];
            int by = curY + SHAPES[curType][curRot][i][1];
            if (by >= 0) drawBlock(g, BOARD_X + bx * BLOCK, BOARD_Y + by * BLOCK, BLOCK, PIECE_COLORS[curType]);
        }
    }

    // Board border
    Pen borderPen(Color(255, 130, 155, 215), 3.0f);
    g.DrawRectangle(&borderPen, BOARD_X - 1, BOARD_Y - 1, BOARD_W + 2, BOARD_H + 2);

    // Panel
    Font labelFont(&fam, 13, FontStyleBold, UnitPixel);
    Font valueFont(&fam, 24, FontStyleBold, UnitPixel);
    Font smallFont(&fam, 12, FontStyleRegular, UnitPixel);
    SolidBrush labelBrush(Color(255, 155, 175, 220));
    SolidBrush valueBrush(Color(255, 245, 248, 255));
    SolidBrush helpBrush (Color(220, 140, 160, 205));

    int py = BOARD_Y;

    g.DrawString(L"NEXT", -1, &labelFont, PointF((REAL)PANEL_X, (REAL)py), &labelBrush);
    py += 22;
    Rect nextR(PANEL_X, py, PANEL_W - 20, 120);
    SolidBrush nextBg(Color(255, 14, 18, 34));
    g.FillRectangle(&nextBg, nextR);
    Pen nextBorder(Color(200, 90, 115, 175), 2.0f);
    g.DrawRectangle(&nextBorder, nextR);

    int minx = 4, miny = 4, maxx = -1, maxy = -1;
    for (int i = 0; i < 4; ++i) {
        int x = SHAPES[nextType][0][i][0];
        int y = SHAPES[nextType][0][i][1];
        if (x < minx) minx = x; if (y < miny) miny = y;
        if (x > maxx) maxx = x; if (y > maxy) maxy = y;
    }
    int nbS = 22;
    int pw = (maxx - minx + 1) * nbS;
    int ph = (maxy - miny + 1) * nbS;
    int ox = PANEL_X + (PANEL_W - 20 - pw) / 2 - minx * nbS;
    int oy = py + (120 - ph) / 2 - miny * nbS;
    for (int i = 0; i < 4; ++i) {
        int x = SHAPES[nextType][0][i][0];
        int y = SHAPES[nextType][0][i][1];
        drawBlock(g, ox + x * nbS, oy + y * nbS, nbS, PIECE_COLORS[nextType]);
    }
    py += 140;

    wchar_t buf[64];
    g.DrawString(L"SCORE", -1, &labelFont, PointF((REAL)PANEL_X, (REAL)py), &labelBrush);
    py += 20; swprintf(buf, 64, L"%d", score);
    g.DrawString(buf, -1, &valueFont, PointF((REAL)PANEL_X, (REAL)py), &valueBrush);
    py += 40;

    g.DrawString(L"LEVEL", -1, &labelFont, PointF((REAL)PANEL_X, (REAL)py), &labelBrush);
    py += 20; swprintf(buf, 64, L"%d", level);
    g.DrawString(buf, -1, &valueFont, PointF((REAL)PANEL_X, (REAL)py), &valueBrush);
    py += 40;

    g.DrawString(L"LINES", -1, &labelFont, PointF((REAL)PANEL_X, (REAL)py), &labelBrush);
    py += 20; swprintf(buf, 64, L"%d", linesCleared);
    g.DrawString(buf, -1, &valueFont, PointF((REAL)PANEL_X, (REAL)py), &valueBrush);
    py += 48;

    const wchar_t* help[] = {
        L"\u2190 \u2192   Move",
        L"\u2191      Rotate",
        L"\u2193      Soft Drop",
        L"Space  Hard Drop",
        L"P      Pause",
        L"R      Restart"
    };
    for (int i = 0; i < 6; ++i)
        g.DrawString(help[i], -1, &smallFont, PointF((REAL)PANEL_X, (REAL)(py + i * 19)), &helpBrush);

    // Overlay
    if (gameOver || paused) {
        SolidBrush overlay(Color(185, 0, 0, 0));
        g.FillRectangle(&overlay, BOARD_X, BOARD_Y, BOARD_W, BOARD_H);
        Font ovFont (&fam, 38, FontStyleBold, UnitPixel);
        Font ovSmall(&fam, 14, FontStyleRegular, UnitPixel);
        SolidBrush ovBrush(Color(255, 255, 244, 244));
        StringFormat ovSf; ovSf.SetAlignment(StringAlignmentCenter);
        REAL cx = (REAL)(BOARD_X + BOARD_W / 2);
        REAL cy = (REAL)(BOARD_Y + BOARD_H / 2);
        if (gameOver) {
            g.DrawString(L"GAME OVER", -1, &ovFont,  PointF(cx, cy - 46), &ovSf, &ovBrush);
            g.DrawString(L"Press R to restart", -1, &ovSmall, PointF(cx, cy + 8), &ovSf, &ovBrush);
        } else {
            g.DrawString(L"PAUSED",    -1, &ovFont,  PointF(cx, cy - 46), &ovSf, &ovBrush);
            g.DrawString(L"Press P to resume", -1, &ovSmall, PointF(cx, cy + 8), &ovSf, &ovBrush);
        }
    }
}

static void ensureBackBuffer(HDC hdc, int w, int h) {
    if (g_backBmp && g_bbW == w && g_bbH == h) return;
    if (g_backBmp) { DeleteObject(g_backBmp); g_backBmp = NULL; }
    if (g_backDC)  { DeleteDC(g_backDC);      g_backDC  = NULL; }
    g_backDC  = CreateCompatibleDC(hdc);
    g_backBmp = CreateCompatibleBitmap(hdc, w, h);
    SelectObject(g_backDC, g_backBmp);
    g_bbW = w; g_bbH = h;
}

// -------- Window --------
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, 1, 16, NULL);
        return 0;

    case WM_TIMER:
        updateGame();
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_KEYDOWN:
        switch (wp) {
        case VK_LEFT:  moveLeft();  break;
        case VK_RIGHT: moveRight(); break;
        case VK_DOWN:  softDrop();  break;
        case VK_UP:    rotate();    break;
        case VK_SPACE: hardDrop();  break;
        case 'P':
            if (!gameOver) {
                paused = !paused;
                if (!paused) lastFallTime = GetTickCount();
            }
            break;
        case 'R': resetGame(); break;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;

    case WM_ERASEBKGND:
        return 1; // we paint everything

    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT rc; GetClientRect(hwnd, &rc);
        int w = rc.right - rc.left, h = rc.bottom - rc.top;
        ensureBackBuffer(hdc, w, h);
        renderGame(g_backDC, w, h);
        BitBlt(hdc, 0, 0, w, h, g_backDC, 0, 0, SRCCOPY);
        EndPaint(hwnd, &ps);
        return 0;
    }

    case WM_GETMINMAXINFO: {
        MINMAXINFO* mmi = (MINMAXINFO*)lp;
        RECT rc = {0, 0, CLIENT_W, CLIENT_H};
        AdjustWindowRect(&rc, WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, FALSE);
        int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
        mmi->ptMinTrackSize.x = ww; mmi->ptMinTrackSize.y = wh;
        mmi->ptMaxTrackSize.x = ww; mmi->ptMaxTrackSize.y = wh;
        return 0;
    }

    case WM_SYSCOMMAND:
        // Block maximize and any snap-like commands just in case
        if ((wp & 0xFFF0) == SC_MAXIMIZE) return 0;
        break;

    case WM_DESTROY:
        KillTimer(hwnd, 1);
        if (g_backBmp) DeleteObject(g_backBmp);
        if (g_backDC)  DeleteDC(g_backDC);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow) {
    srand((unsigned)time(NULL));
    initColors();

    GdiplusStartupInput si;
    GdiplusStartup(&g_gdipToken, &si, NULL);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = NULL;
    wc.lpszClassName = L"TetrisWnd";
    wc.hIcon         = LoadIcon(NULL, IDI_APPLICATION);
    RegisterClassExW(&wc);

    // No WS_THICKFRAME and no WS_MAXIMIZEBOX => not resizable, not snappable,
    // no Snap Layouts flyout on the (absent) maximize button.
    DWORD style = WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
    RECT rc = {0, 0, CLIENT_W, CLIENT_H};
    AdjustWindowRect(&rc, style, FALSE);
    int ww = rc.right - rc.left;
    int wh = rc.bottom - rc.top;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - ww) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - wh) / 2;

    resetGame();

    g_hwnd = CreateWindowExW(0, L"TetrisWnd", L"Tetris",
                             style, sx, sy, ww, wh, NULL, NULL, hInst, NULL);
    if (!g_hwnd) { GdiplusShutdown(g_gdipToken); return 0; }

    ShowWindow(g_hwnd, nShow);
    UpdateWindow(g_hwnd);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    GdiplusShutdown(g_gdipToken);
    return 0;
}