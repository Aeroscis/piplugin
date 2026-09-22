// drag_measure.cs — Win32 helpers for the automated border-drag measurement.
// Used by drag_measure.ps1 (Add-Type). C# 5 compatible (PowerShell 5.1 compiler).
using System;
using System.Runtime.InteropServices;

public static class Dm
{
    [StructLayout(LayoutKind.Sequential)]
    public struct RECT { public int L; public int T; public int R; public int B; }

    [StructLayout(LayoutKind.Sequential)]
    public struct POINT { public int X; public int Y; }

    [StructLayout(LayoutKind.Sequential)]
    public struct BITMAPINFOHEADER
    {
        public uint biSize;
        public int biWidth;
        public int biHeight;
        public ushort biPlanes;
        public ushort biBitCount;
        public uint biCompression;
        public uint biSizeImage;
        public int biXPelsPerMeter;
        public int biYPelsPerMeter;
        public uint biClrUsed;
        public uint biClrImportant;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct RGBQUAD { public byte B; public byte G; public byte R; public byte A; }

    [StructLayout(LayoutKind.Sequential)]
    public struct BITMAPINFO
    {
        public BITMAPINFOHEADER Header;
        public RGBQUAD Colors;
    }

    [DllImport("user32.dll")]
    public static extern bool SetProcessDPIAware();

    [DllImport("user32.dll", CharSet = CharSet.Unicode)]
    public static extern IntPtr FindWindowW(string lpClassName, string lpWindowName);

    // PowerShell marshals $null as "" for string parameters, which makes
    // FindWindow look for a window with an empty title - wrap it instead.
    public static IntPtr FindHostWindow()
    {
        return FindWindowW("PiTestHost", null);
    }

    [DllImport("user32.dll")]
    public static extern bool GetWindowRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    public static extern bool GetClientRect(IntPtr hWnd, out RECT lpRect);

    [DllImport("user32.dll")]
    public static extern bool ClientToScreen(IntPtr hWnd, ref POINT lpPoint);

    [DllImport("user32.dll")]
    public static extern bool SetCursorPos(int X, int Y);

    [DllImport("user32.dll")]
    public static extern void mouse_event(uint dwFlags, uint dx, uint dy, uint dwData, UIntPtr dwExtraInfo);

    [DllImport("user32.dll")]
    public static extern IntPtr GetDC(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern int ReleaseDC(IntPtr hWnd, IntPtr hDC);

    [DllImport("user32.dll")]
    public static extern bool SetWindowPos(IntPtr hWnd, IntPtr hWndInsertAfter, int X, int Y, int cx, int cy, uint uFlags);

    [DllImport("user32.dll")]
    public static extern bool SetForegroundWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern bool IsWindow(IntPtr hWnd);

    [DllImport("user32.dll")]
    public static extern int GetSystemMetrics(int nIndex);

    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateCompatibleDC(IntPtr hdc);

    [DllImport("gdi32.dll")]
    public static extern IntPtr CreateCompatibleBitmap(IntPtr hdc, int nWidth, int nHeight);

    [DllImport("gdi32.dll")]
    public static extern IntPtr SelectObject(IntPtr hdc, IntPtr hgdiobj);

    [DllImport("gdi32.dll")]
    public static extern bool DeleteObject(IntPtr hObject);

    [DllImport("gdi32.dll")]
    public static extern bool DeleteDC(IntPtr hdc);

    [DllImport("gdi32.dll")]
    public static extern bool BitBlt(IntPtr hdcDest, int nXDest, int nYDest, int nWidth, int nHeight, IntPtr hdcSrc, int nXSrc, int nYSrc, uint dwRop);

    [DllImport("gdi32.dll")]
    public static extern int GetDIBits(IntPtr hdc, IntPtr hbmp, uint uStartScan, uint cScanLines, byte[] lpvBits, ref BITMAPINFO lpbi, uint uUsage);

    // Captures one scan line of the host window's client area straight off the
    // composited screen and locates the ImGui panel edge and the plugin area
    // edge on it.
    //   panelW:   width of the dark [16,16,16] ImGui panel (right-most dark run,
    //             measured from client x=0). -1 when not found.
    //   pluginLeft: first x of the bright [241,241,241] Qt plugin area. -1 when
    //             not found (plugin not loaded).
    // Returns 0 on success, non-zero on Win32 failure.
    public static int CaptureRow(IntPtr hwnd, double yFrac, out int clientW, out int clientH, out int panelW, out int pluginLeft)
    {
        clientW = 0; clientH = 0; panelW = -1; pluginLeft = -1;

        RECT cr;
        if (!GetClientRect(hwnd, out cr)) return 1;
        clientW = cr.R; clientH = cr.B;
        if (clientW <= 0 || clientH <= 0) return 2;

        POINT org = new POINT(); org.X = 0; org.Y = 0;
        if (!ClientToScreen(hwnd, ref org)) return 3;
        int rowY = org.Y + (int)(clientH * yFrac);

        IntPtr screen = GetDC(IntPtr.Zero);
        if (screen == IntPtr.Zero) return 4;
        IntPtr mem = CreateCompatibleDC(screen);
        IntPtr bmp = CreateCompatibleBitmap(screen, clientW, 1);
        IntPtr old = SelectObject(mem, bmp);

        int res = 0;
        bool ok = BitBlt(mem, 0, 0, clientW, 1, screen, org.X, rowY, 0x00CC0020u /* SRCCOPY */);
        if (!ok) { res = 5; }
        else
        {
            BITMAPINFO bi = new BITMAPINFO();
            bi.Header.biSize = (uint)Marshal.SizeOf(typeof(BITMAPINFOHEADER));
            bi.Header.biWidth = clientW;
            bi.Header.biHeight = -1;      // top-down, exactly the one row
            bi.Header.biPlanes = 1;
            bi.Header.biBitCount = 32;
            bi.Header.biCompression = 0;  // BI_RGB
            byte[] bits = new byte[clientW * 4];
            int got = GetDIBits(mem, bmp, 0, 1, bits, ref bi, 0 /* DIB_RGB_COLORS */);
            if (got != 1) { res = 6; }
            else
            {
                int deepRight = -1;
                for (int x = clientW - 1; x >= 0; --x)
                {
                    byte b = bits[x * 4], g = bits[x * 4 + 1], r = bits[x * 4 + 2];
                    if (Math.Abs(b - 16) <= 13 && Math.Abs(g - 16) <= 13 && Math.Abs(r - 16) <= 13)
                    { deepRight = x; break; }
                }
                int brightLeft = -1;
                for (int x = 0; x < clientW; ++x)
                {
                    byte b = bits[x * 4], g = bits[x * 4 + 1], r = bits[x * 4 + 2];
                    if (Math.Abs(b - 241) <= 13 && Math.Abs(g - 241) <= 13 && Math.Abs(r - 241) <= 13)
                    { brightLeft = x; break; }
                }
                if (deepRight >= 0) panelW = deepRight + 1;
                pluginLeft = brightLeft;
            }
        }

        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(IntPtr.Zero, screen);
        return res;
    }
}
