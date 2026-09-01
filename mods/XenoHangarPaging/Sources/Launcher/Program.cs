using System;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;

namespace XenoHangarPaging
{
    internal static class Program
    {
        private const string ModToken = @"XenoMods\XenoHangarPaging";
        private const string RuntimeFileName = "XenoHangarPaging.Runtime.dll";
        private const uint CreateSuspended = 0x00000004;
        private const uint MemCommit = 0x00001000;
        private const uint MemReserve = 0x00002000;
        private const uint PageReadWrite = 0x04;
        private const uint MemRelease = 0x00008000;
        private const uint Infinite = 0xffffffff;
        private const uint WaitObject0 = 0;

        [STAThread]
        private static int Main(string[] args)
        {
            string launcherDirectory = AppDomain.CurrentDomain.BaseDirectory.TrimEnd(
                Path.DirectorySeparatorChar,
                Path.AltDirectorySeparatorChar);
            string logPath = Path.Combine(launcherDirectory, "XenoHangarPaging.log");
            bool selfTest = args.Any(value => string.Equals(value, "--self-test", StringComparison.OrdinalIgnoreCase));

            try
            {
                Options options = ParseOptions(args);
                string gameExe = FindGameExecutable(launcherDirectory, options.GamePath);
                string gameDirectory = Path.GetDirectoryName(gameExe);
                string runtimePath = Path.Combine(launcherDirectory, RuntimeFileName);

                if (!File.Exists(runtimePath))
                {
                    throw new FileNotFoundException("Не найдена runtime-библиотека мода.", runtimePath);
                }
                ValidateGameExecutable(gameExe);
                if (!options.SelfTest && !IsModEnabled(gameDirectory))
                {
                    throw new InvalidOperationException(
                        "Сначала включите XenoHangarPaging в менеджере модов. " +
                        "Ожидаемая запись: " + ModToken + ".");
                }

                Log(logPath, "Launcher 1.0.0; game=" + gameExe + "; SHA-256=" + ComputeSha256(gameExe));
                LaunchAndInject(
                    gameExe,
                    gameDirectory,
                    runtimePath,
                    options.ForwardedArguments,
                    options.SelfTest);
                Log(logPath, options.SelfTest
                    ? "Self-test passed; suspended test process terminated."
                    : "Runtime loaded; game resumed.");
                return 0;
            }
            catch (Exception exception)
            {
                Log(logPath, "ERROR: " + exception);
                if (!selfTest)
                {
                    MessageBoxW(
                        IntPtr.Zero,
                        exception.Message + "\n\nФайлы игры не изменялись. Подробности:\n" + logPath,
                        "XenoHangarPaging",
                        0x00000010);
                }
                return 1;
            }
        }

        private static Options ParseOptions(string[] args)
        {
            Options result = new Options();
            for (int index = 0; index < args.Length; ++index)
            {
                string argument = args[index];
                if (string.Equals(argument, "--self-test", StringComparison.OrdinalIgnoreCase))
                {
                    result.SelfTest = true;
                }
                else if (string.Equals(argument, "--game", StringComparison.OrdinalIgnoreCase))
                {
                    if (++index >= args.Length)
                    {
                        throw new ArgumentException("После --game нужно указать Rangers.exe или папку игры.");
                    }
                    result.GamePath = args[index];
                }
                else if (argument.StartsWith("--game=", StringComparison.OrdinalIgnoreCase))
                {
                    result.GamePath = argument.Substring("--game=".Length);
                }
                else
                {
                    result.ForwardedArguments = AppendArgument(result.ForwardedArguments, argument);
                }
            }
            return result;
        }

        private static string AppendArgument(string commandLine, string argument)
        {
            string quoted = QuoteArgument(argument);
            return string.IsNullOrEmpty(commandLine) ? quoted : commandLine + " " + quoted;
        }

        private static string FindGameExecutable(string startDirectory, string explicitPath)
        {
            if (!string.IsNullOrWhiteSpace(explicitPath))
            {
                string full = Path.GetFullPath(explicitPath);
                if (Directory.Exists(full))
                {
                    full = Path.Combine(full, "Rangers.exe");
                }
                if (!File.Exists(full))
                {
                    throw new FileNotFoundException("Не найден Rangers.exe.", full);
                }
                return full;
            }

            DirectoryInfo directory = new DirectoryInfo(startDirectory);
            for (int depth = 0; directory != null && depth < 9; ++depth, directory = directory.Parent)
            {
                string candidate = Path.Combine(directory.FullName, "Rangers.exe");
                if (File.Exists(candidate))
                {
                    return candidate;
                }
            }
            throw new FileNotFoundException(
                "Rangers.exe не найден. Поместите мод в Mods\\OtherMods или укажите --game.");
        }

        private static void ValidateGameExecutable(string path)
        {
            using (FileStream stream = File.OpenRead(path))
            using (BinaryReader reader = new BinaryReader(stream))
            {
                if (reader.ReadUInt16() != 0x5a4d)
                {
                    throw new InvalidDataException("Rangers.exe не является PE-файлом.");
                }
                stream.Position = 0x3c;
                int peOffset = reader.ReadInt32();
                stream.Position = peOffset;
                if (reader.ReadUInt32() != 0x00004550 || reader.ReadUInt16() != 0x014c)
                {
                    throw new InvalidDataException("Нужен 32-битный Rangers.exe версии 2.1.2500.0.");
                }
                stream.Position = peOffset + 24;
                if (reader.ReadUInt16() != 0x010b)
                {
                    throw new InvalidDataException("Нужен 32-битный Rangers.exe (PE32).");
                }
            }

            FileVersionInfo version = FileVersionInfo.GetVersionInfo(path);
            if (version.FileMajorPart != 2 || version.FileMinorPart != 1 || version.FileBuildPart != 2500)
            {
                throw new InvalidDataException(
                    "Поддерживается Space Rangers HD 2.1.2500.0; обнаружена версия " +
                    version.FileVersion + ".");
            }
        }

        private static bool IsModEnabled(string gameDirectory)
        {
            string path = Path.Combine(gameDirectory, "Mods", "ModCFG.txt");
            if (!File.Exists(path))
            {
                return false;
            }
            foreach (string line in File.ReadAllLines(path, Encoding.Default))
            {
                const string prefix = "CurrentMod=";
                if (!line.StartsWith(prefix, StringComparison.OrdinalIgnoreCase))
                {
                    continue;
                }
                return line.Substring(prefix.Length)
                    .Split(',')
                    .Any(value => string.Equals(value.Trim(), ModToken, StringComparison.OrdinalIgnoreCase));
            }
            return false;
        }

        private static void LaunchAndInject(
            string gameExe,
            string gameDirectory,
            string runtimePath,
            string forwardedArguments,
            bool selfTest)
        {
            STARTUPINFO startup = new STARTUPINFO();
            startup.cb = Marshal.SizeOf(typeof(STARTUPINFO));
            PROCESS_INFORMATION process;
            StringBuilder commandLine = new StringBuilder(QuoteArgument(gameExe));
            if (!string.IsNullOrWhiteSpace(forwardedArguments))
            {
                commandLine.Append(' ').Append(forwardedArguments);
            }

            if (!CreateProcessW(
                gameExe,
                commandLine,
                IntPtr.Zero,
                IntPtr.Zero,
                false,
                CreateSuspended,
                IntPtr.Zero,
                gameDirectory,
                ref startup,
                out process))
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Не удалось запустить Rangers.exe.");
            }

            bool keepProcess = false;
            try
            {
                InjectLibrary(process.hProcess, runtimePath);
                if (selfTest)
                {
                    if (!TerminateProcess(process.hProcess, 0))
                    {
                        throw new Win32Exception(Marshal.GetLastWin32Error(), "Не удалось завершить тестовый процесс.");
                    }
                    WaitForSingleObject(process.hProcess, 10000);
                    return;
                }
                if (ResumeThread(process.hThread) == 0xffffffff)
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Не удалось продолжить игру.");
                }
                keepProcess = true;
            }
            finally
            {
                if (!keepProcess && !selfTest)
                {
                    TerminateProcess(process.hProcess, 1);
                }
                CloseHandle(process.hThread);
                CloseHandle(process.hProcess);
            }
        }

        private static void InjectLibrary(IntPtr process, string runtimePath)
        {
            byte[] pathBytes = Encoding.Unicode.GetBytes(Path.GetFullPath(runtimePath) + "\0");
            IntPtr remotePath = VirtualAllocEx(
                process,
                IntPtr.Zero,
                new UIntPtr((uint)pathBytes.Length),
                MemCommit | MemReserve,
                PageReadWrite);
            if (remotePath == IntPtr.Zero)
            {
                throw new Win32Exception(Marshal.GetLastWin32Error(), "Не удалось выделить память для runtime.");
            }

            try
            {
                UIntPtr written;
                if (!WriteProcessMemory(process, remotePath, pathBytes, pathBytes.Length, out written) ||
                    written.ToUInt32() != pathBytes.Length)
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Не удалось передать runtime в процесс игры.");
                }

                IntPtr kernel32 = GetModuleHandleW("kernel32.dll");
                IntPtr loadLibrary = GetProcAddress(kernel32, "LoadLibraryW");
                if (kernel32 == IntPtr.Zero || loadLibrary == IntPtr.Zero)
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Не найден LoadLibraryW.");
                }

                IntPtr thread = CreateRemoteThread(
                    process,
                    IntPtr.Zero,
                    UIntPtr.Zero,
                    loadLibrary,
                    remotePath,
                    0,
                    IntPtr.Zero);
                if (thread == IntPtr.Zero)
                {
                    throw new Win32Exception(Marshal.GetLastWin32Error(), "Не удалось загрузить runtime в игру.");
                }
                try
                {
                    if (WaitForSingleObject(thread, 10000) != WaitObject0)
                    {
                        throw new TimeoutException("Runtime не загрузился за 10 секунд.");
                    }
                    uint moduleHandle;
                    if (!GetExitCodeThread(thread, out moduleHandle) || moduleHandle == 0)
                    {
                        throw new InvalidOperationException(
                            "Runtime отклонил Rangers.exe: сигнатуры версии 2.1.2500.0 не совпали.");
                    }
                }
                finally
                {
                    CloseHandle(thread);
                }
            }
            finally
            {
                VirtualFreeEx(process, remotePath, UIntPtr.Zero, MemRelease);
            }
        }

        private static string QuoteArgument(string value)
        {
            if (value == null)
            {
                return "\"\"";
            }
            return "\"" + value.Replace("\"", "\\\"") + "\"";
        }

        private static string ComputeSha256(string path)
        {
            using (SHA256 hash = SHA256.Create())
            using (FileStream stream = File.OpenRead(path))
            {
                return BitConverter.ToString(hash.ComputeHash(stream)).Replace("-", string.Empty);
            }
        }

        private static void Log(string path, string message)
        {
            File.AppendAllText(
                path,
                DateTime.Now.ToString("yyyy-MM-dd HH:mm:ss") + " " + message + Environment.NewLine,
                Encoding.UTF8);
        }

        private sealed class Options
        {
            public string GamePath;
            public string ForwardedArguments;
            public bool SelfTest;
        }

        [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
        private struct STARTUPINFO
        {
            public int cb;
            public string lpReserved;
            public string lpDesktop;
            public string lpTitle;
            public int dwX;
            public int dwY;
            public int dwXSize;
            public int dwYSize;
            public int dwXCountChars;
            public int dwYCountChars;
            public int dwFillAttribute;
            public int dwFlags;
            public short wShowWindow;
            public short cbReserved2;
            public IntPtr lpReserved2;
            public IntPtr hStdInput;
            public IntPtr hStdOutput;
            public IntPtr hStdError;
        }

        [StructLayout(LayoutKind.Sequential)]
        private struct PROCESS_INFORMATION
        {
            public IntPtr hProcess;
            public IntPtr hThread;
            public int dwProcessId;
            public int dwThreadId;
        }

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern bool CreateProcessW(
            string applicationName,
            StringBuilder commandLine,
            IntPtr processAttributes,
            IntPtr threadAttributes,
            bool inheritHandles,
            uint creationFlags,
            IntPtr environment,
            string currentDirectory,
            ref STARTUPINFO startupInfo,
            out PROCESS_INFORMATION processInformation);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr VirtualAllocEx(
            IntPtr process,
            IntPtr address,
            UIntPtr size,
            uint allocationType,
            uint protection);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool VirtualFreeEx(IntPtr process, IntPtr address, UIntPtr size, uint freeType);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool WriteProcessMemory(
            IntPtr process,
            IntPtr baseAddress,
            byte[] buffer,
            int size,
            out UIntPtr bytesWritten);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern IntPtr CreateRemoteThread(
            IntPtr process,
            IntPtr threadAttributes,
            UIntPtr stackSize,
            IntPtr startAddress,
            IntPtr parameter,
            uint creationFlags,
            IntPtr threadId);

        [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
        private static extern IntPtr GetModuleHandleW(string moduleName);

        [DllImport("kernel32.dll", CharSet = CharSet.Ansi, SetLastError = true)]
        private static extern IntPtr GetProcAddress(IntPtr module, string procName);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint WaitForSingleObject(IntPtr handle, uint milliseconds);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool GetExitCodeThread(IntPtr thread, out uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern uint ResumeThread(IntPtr thread);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool TerminateProcess(IntPtr process, uint exitCode);

        [DllImport("kernel32.dll", SetLastError = true)]
        private static extern bool CloseHandle(IntPtr handle);

        [DllImport("user32.dll", CharSet = CharSet.Unicode)]
        private static extern int MessageBoxW(IntPtr window, string text, string caption, uint type);
    }
}
