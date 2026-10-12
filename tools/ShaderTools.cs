// Helpers for tools/gen_twins.ps1: DXIL disassembly through the game's own dxcompiler.dll, and a sequence diff.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;

namespace CamFx
{
    [ComImport, Guid("8BA5FB08-5195-40e2-AC58-0D989C3A0102"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDxcBlob
    {
        [PreserveSig] IntPtr GetBufferPointer();
        [PreserveSig] UIntPtr GetBufferSize();
    }

    [ComImport, Guid("7241d424-2646-4191-97c0-98e96e42fc68"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDxcBlobEncoding
    {
        [PreserveSig] IntPtr GetBufferPointer();
        [PreserveSig] UIntPtr GetBufferSize();
        [PreserveSig] int GetEncoding(out int known, out uint codePage);
    }

    [ComImport, Guid("e5204dc7-d18c-4c3c-bdfb-851673980fe7"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDxcLibrary
    {
        [PreserveSig] int SetMalloc(IntPtr pMalloc);
        [PreserveSig] int CreateBlobFromBlob(IntPtr pBlob, uint offset, uint length, out IntPtr ppResult);
        [PreserveSig] int CreateBlobFromFile(IntPtr pFileName, IntPtr codePage, out IntPtr pBlobEncoding);
        [PreserveSig] int CreateBlobWithEncodingFromPinned(IntPtr pText, uint size, uint codePage, out IntPtr pBlobEncoding);
        [PreserveSig] int CreateBlobWithEncodingOnHeapCopy(IntPtr pText, uint size, uint codePage, [MarshalAs(UnmanagedType.Interface)] out IDxcBlobEncoding pBlobEncoding);
    }

    [ComImport, Guid("8c210bf3-011f-4422-8d70-6f9acb8db617"), InterfaceType(ComInterfaceType.InterfaceIsIUnknown)]
    public interface IDxcCompiler
    {
        [PreserveSig] int Compile(IntPtr a, IntPtr b, IntPtr c, IntPtr d, IntPtr e, uint f, IntPtr g, uint h, IntPtr i, out IntPtr j);
        [PreserveSig] int Preprocess(IntPtr a, IntPtr b, IntPtr c, uint d, IntPtr e, uint f, IntPtr g, out IntPtr h);
        [PreserveSig] int Disassemble([MarshalAs(UnmanagedType.Interface)] IDxcBlob pSource, [MarshalAs(UnmanagedType.Interface)] out IDxcBlobEncoding ppDisassembly);
    }

    public static class Dxc
    {
        [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Unicode)]
        static extern IntPtr LoadLibraryW(string path);
        [DllImport("kernel32", SetLastError = true, CharSet = CharSet.Ansi, BestFitMapping = false)]
        static extern IntPtr GetProcAddress(IntPtr module, string name);

        [UnmanagedFunctionPointer(CallingConvention.StdCall)]
        delegate int CreateInstance(ref Guid clsid, ref Guid iid, out IntPtr result);

        static IDxcLibrary library;
        static IDxcCompiler compiler;

        public static void Init(string dll)
        {
            IntPtr module = LoadLibraryW(dll);
            if (module == IntPtr.Zero) throw new Exception("LoadLibrary failed: " + Marshal.GetLastWin32Error());
            IntPtr proc = GetProcAddress(module, "DxcCreateInstance");
            if (proc == IntPtr.Zero) throw new Exception("DxcCreateInstance not exported");
            var create = (CreateInstance)Marshal.GetDelegateForFunctionPointer(proc, typeof(CreateInstance));

            Guid clsLibrary = new Guid("6245d6af-66e0-48fd-80b4-4d271796748c"), iidLibrary = typeof(IDxcLibrary).GUID;
            Guid clsCompiler = new Guid("73e22d93-e6ce-47f3-b5bf-f0664f39c1b0"), iidCompiler = typeof(IDxcCompiler).GUID;
            IntPtr result;
            int hr = create(ref clsLibrary, ref iidLibrary, out result);
            if (hr < 0) throw new Exception("DxcLibrary hr=0x" + hr.ToString("X8"));
            library = (IDxcLibrary)Marshal.GetObjectForIUnknown(result); Marshal.Release(result);
            hr = create(ref clsCompiler, ref iidCompiler, out result);
            if (hr < 0) throw new Exception("DxcCompiler hr=0x" + hr.ToString("X8"));
            compiler = (IDxcCompiler)Marshal.GetObjectForIUnknown(result); Marshal.Release(result);
        }

        public static string Disassemble(byte[] data, int offset, int size)
        {
            IntPtr memory = Marshal.AllocHGlobal(size);
            try
            {
                Marshal.Copy(data, offset, memory, size);
                IDxcBlobEncoding source;
                int hr = library.CreateBlobWithEncodingOnHeapCopy(memory, (uint)size, 0, out source);
                if (hr < 0 || source == null) throw new Exception("CreateBlob hr=0x" + hr.ToString("X8"));
                IDxcBlobEncoding text;
                hr = compiler.Disassemble((IDxcBlob)source, out text);
                if (hr < 0 || text == null) throw new Exception("Disassemble hr=0x" + hr.ToString("X8"));
                int length = (int)text.GetBufferSize().ToUInt64();
                byte[] bytes = new byte[length];
                Marshal.Copy(text.GetBufferPointer(), bytes, 0, length);
                Marshal.ReleaseComObject(text);
                Marshal.ReleaseComObject(source);
                return Encoding.UTF8.GetString(bytes).TrimEnd('\0');
            }
            finally { Marshal.FreeHGlobal(memory); }
        }
    }

    public static class Seq
    {
        static readonly Regex Ssa = new Regex(@"%\d+", RegexOptions.Compiled);
        static readonly Regex Meta = new Regex(@"!\d+", RegexOptions.Compiled);
        static readonly Dictionary<string, int> Ids = new Dictionary<string, int>();

        // Function body as a sequence of line ids, with SSA value and metadata numbers removed so equal code compares equal.
        public static int[] Build(string text)
        {
            var list = new List<int>();
            int start = text.IndexOf("define void @");
            int end = start < 0 ? -1 : text.IndexOf("\n}", start);
            if (start < 0 || end < 0) return list.ToArray();
            foreach (var raw in text.Substring(start, end - start).Split('\n'))
            {
                var line = raw.Trim();
                if (line.Length == 0 || line.StartsWith(";") || line.StartsWith("define")) continue;
                int comment = line.IndexOf(" ; ");
                if (comment >= 0) line = line.Substring(0, comment);
                line = Meta.Replace(Ssa.Replace(line, "%"), "!");
                int id;
                if (!Ids.TryGetValue(line, out id)) { id = Ids.Count; Ids[line] = id; }
                list.Add(id);
            }
            return list.ToArray();
        }

        // Myers O(ND) shortest edit script length, or -1 beyond maxD.
        public static int EditDistance(int[] a, int[] b, int maxD)
        {
            int n = a.Length, m = b.Length, off = maxD + 1;
            var v = new int[2 * maxD + 3];
            for (int d = 0; d <= maxD; d++)
            {
                for (int k = -d; k <= d; k += 2)
                {
                    int x = (k == -d || (k != d && v[off + k - 1] < v[off + k + 1])) ? v[off + k + 1] : v[off + k - 1] + 1;
                    int y = x - k;
                    while (x < n && y < m && a[x] == b[y]) { x++; y++; }
                    v[off + k] = x;
                    if (x >= n && y >= m) return d;
                }
            }
            return -1;
        }
    }
}
