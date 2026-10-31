using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Accessibility;

internal static class MsaaProbe
{
    private const uint ObjidClient = 0xFFFFFFFC;

    [StructLayout(LayoutKind.Sequential)]
    private struct Point
    {
        public int X;
        public int Y;
    }

    [DllImport("oleacc.dll")]
    private static extern int AccessibleObjectFromWindow(
        IntPtr hwnd, uint id, ref Guid iid,
        [MarshalAs(UnmanagedType.Interface)] out IAccessible accessible);

    [DllImport("oleacc.dll")]
    private static extern int AccessibleObjectFromPoint(
        Point point, [MarshalAs(UnmanagedType.Interface)] out IAccessible accessible,
        [MarshalAs(UnmanagedType.Struct)] out object child);

    [DllImport("oleacc.dll")]
    private static extern int AccessibleChildren(
        [MarshalAs(UnmanagedType.Interface)] IAccessible parent, int start, int count,
        [Out, MarshalAs(UnmanagedType.LPArray, SizeParamIndex = 3)] object[] children,
        out int obtained);

    private static int Main(string[] args)
    {
        if (args.Length < 1)
            return 2;

        IntPtr hwnd = new IntPtr(long.Parse(args[0]));
        Guid iid = new Guid("618736E0-3C3D-11CF-810C-00AA00389B71");
        IAccessible root;
        int hr = AccessibleObjectFromWindow(hwnd, ObjidClient, ref iid, out root);
        Console.WriteLine("root hr=" + hr);
        if (hr != 0 || root == null)
            return 1;

        if (args.Length >= 4)
        {
            Point point = new Point { X = int.Parse(args[1]), Y = int.Parse(args[2]) };
            IAccessible hit;
            object childValue;
            hr = AccessibleObjectFromPoint(point, out hit, out childValue);
            Console.WriteLine("hit hr=" + hr + " child=" + childValue + " type=" +
                              (hit == null ? "null" : hit.GetType().FullName));
            if (hit != null)
                Describe(hit, 0, "hit", 0);
            return hr == 0 ? 0 : 1;
        }

        int maxDepth = args.Length >= 2 ? int.Parse(args[1]) : 8;
        int maxNodes = args.Length >= 3 ? int.Parse(args[2]) : 500;
        int count = 0;
        Walk(root, 0, "0", maxDepth, maxNodes, ref count);
        return 0;
    }

    private static void Walk(IAccessible accessible, int depth, string path,
                             int maxDepth, int maxNodes, ref int count)
    {
        if (accessible == null || depth > maxDepth || count >= maxNodes)
            return;
        count++;
        Describe(accessible, depth, path, 0);

        int childCount;
        try
        {
            childCount = accessible.accChildCount;
        }
        catch
        {
            return;
        }
        if (childCount <= 0)
            return;

        object[] children = new object[childCount];
        int obtained;
        try
        {
            AccessibleChildren(accessible, 0, childCount, children, out obtained);
        }
        catch (Exception error)
        {
            Console.WriteLine(path + " enumerate-error=" + error.Message);
            return;
        }

        for (int index = 0; index < obtained && count < maxNodes; index++)
        {
            IAccessible child = children[index] as IAccessible;
            if (child != null)
                Walk(child, depth + 1, path + "/" + index, maxDepth, maxNodes, ref count);
            else
                Console.WriteLine(path + "/" + index + " simple=" + children[index]);
        }
    }

    private static void Describe(IAccessible accessible, int depth, string path, object child)
    {
        string name = "";
        string action = "";
        object role = "";
        object state = "";
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
        try { name = accessible.get_accName(child) ?? ""; } catch { }
        try { role = accessible.get_accRole(child) ?? ""; } catch { }
        try { state = accessible.get_accState(child) ?? ""; } catch { }
        try { action = accessible.get_accDefaultAction(child) ?? ""; } catch { }
        try { accessible.accLocation(out left, out top, out width, out height, child); } catch { }
        Console.WriteLine(path + " depth=" + depth + " name='" + name + "' role=" + role +
                          " state=" + state + " action='" + action + "' rect=" +
                          left + "," + top + "," + width + "," + height);
    }
}
