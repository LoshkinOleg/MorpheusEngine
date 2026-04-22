using System.ComponentModel;
using System.Diagnostics;
using System.Runtime.InteropServices;
using Microsoft.Win32.SafeHandles;

namespace MorpheusEngine
{
    /// <summary>
    /// Minimal Windows Job Object wrapper used to guarantee that the Ollama child process dies if this provider
    /// process exits unexpectedly. The helper is intentionally local to the qwen provider because no other module
    /// currently needs child-process kill-on-close semantics.
    /// </summary>
    internal sealed class WindowsJobObject : IDisposable
    {
        private readonly SafeJobHandle _handle;
        private bool _disposed;

        public WindowsJobObject()
        {
            _handle = NativeMethods.CreateJobObject(IntPtr.Zero, null);
            if (_handle.IsInvalid)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to create Windows Job Object.");
            }

            var limits = new JOBOBJECT_EXTENDED_LIMIT_INFORMATION
            {
                BasicLimitInformation = new JOBOBJECT_BASIC_LIMIT_INFORMATION
                {
                    LimitFlags = NativeMethods.JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
                }
            };

            var infoSize = Marshal.SizeOf<JOBOBJECT_EXTENDED_LIMIT_INFORMATION>();
            var infoPtr = Marshal.AllocHGlobal(infoSize);
            try
            {
                Marshal.StructureToPtr(limits, infoPtr, false);
                if (!NativeMethods.SetInformationJobObject(
                        _handle,
                        NativeMethods.JobObjectExtendedLimitInformation,
                        infoPtr,
                        (uint)infoSize))
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to configure Windows Job Object.");
                }
            }
            finally
            {
                Marshal.FreeHGlobal(infoPtr);
            }
        }

        public void AssignProcess(Process process)
        {
            ObjectDisposedException.ThrowIf(_disposed, this);
            ArgumentNullException.ThrowIfNull(process);
            if (process.HasExited)
            {
                throw new InvalidOperationException("Cannot assign an exited process to the Windows Job Object.");
            }

            if (!NativeMethods.AssignProcessToJobObject(_handle, process.Handle))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Failed to assign child process to Windows Job Object.");
            }
        }

        public void Dispose()
        {
            if (_disposed)
            {
                return;
            }

            _disposed = true;
            _handle.Dispose();
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JOBOBJECT_BASIC_LIMIT_INFORMATION
        {
            public long PerProcessUserTimeLimit;
            public long PerJobUserTimeLimit;
            public uint LimitFlags;
            public UIntPtr MinimumWorkingSetSize;
            public UIntPtr MaximumWorkingSetSize;
            public uint ActiveProcessLimit;
            public UIntPtr Affinity;
            public uint PriorityClass;
            public uint SchedulingClass;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct IO_COUNTERS
        {
            public ulong ReadOperationCount;
            public ulong WriteOperationCount;
            public ulong OtherOperationCount;
            public ulong ReadTransferCount;
            public ulong WriteTransferCount;
            public ulong OtherTransferCount;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct JOBOBJECT_EXTENDED_LIMIT_INFORMATION
        {
            public JOBOBJECT_BASIC_LIMIT_INFORMATION BasicLimitInformation;
            public IO_COUNTERS IoInfo;
            public UIntPtr ProcessMemoryLimit;
            public UIntPtr JobMemoryLimit;
            public UIntPtr PeakProcessMemoryUsed;
            public UIntPtr PeakJobMemoryUsed;
        }

        private sealed class SafeJobHandle : SafeHandleZeroOrMinusOneIsInvalid
        {
            public SafeJobHandle()
                : base(true)
            {
            }

            protected override bool ReleaseHandle() => NativeMethods.CloseHandle(handle);
        }

        private static class NativeMethods
        {
            public const uint JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000;
            public const int JobObjectExtendedLimitInformation = 9;

            [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
            public static extern SafeJobHandle CreateJobObject(IntPtr lpJobAttributes, string? lpName);

            [DllImport("kernel32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool SetInformationJobObject(
                SafeJobHandle hJob,
                int jobObjectInfoClass,
                IntPtr lpJobObjectInfo,
                uint cbJobObjectInfoLength);

            [DllImport("kernel32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool AssignProcessToJobObject(SafeJobHandle job, IntPtr process);

            [DllImport("kernel32.dll", SetLastError = true)]
            [return: MarshalAs(UnmanagedType.Bool)]
            public static extern bool CloseHandle(IntPtr hObject);
        }
    }
}
