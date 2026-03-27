using System.Diagnostics;

namespace MorpheusEngine
{
    public abstract class Module
    {
        public abstract void Run();

        public virtual void Stop(){}
    }

    public sealed class RouterModule : Module
    {
        private Process _process = new Process();

        public override void Run()
        {
            string dotnetRoot = "";

            var dir = new DirectoryInfo(AppContext.BaseDirectory);
            while (dir is not null)
            {
                if (File.Exists(Path.Combine(dir.FullName, "MorpheusEngine.sln")))
                {
                    dotnetRoot = dir.FullName;
                }
                dir = dir.Parent;
            }
            if (dotnetRoot == "")
            {
                dotnetRoot = Environment.CurrentDirectory;
            }

            var projectPath = Path.Combine(dotnetRoot, "src", "MorpheusEngine.RouterModule", "MorpheusEngine.RouterModule.csproj");
            var psi = new ProcessStartInfo
            {
                FileName = "dotnet",
                Arguments = $"run --project \"{projectPath}\" --",
                WorkingDirectory = dotnetRoot,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };

            _process = Process.Start(psi) ?? throw new InvalidOperationException("Failed to start RouterModule process.");
            _process.OutputDataReceived += (_, e) =>
            {
                if (!string.IsNullOrWhiteSpace(e.Data))
                {
                    Console.WriteLine("[Router] " + e.Data);
                }
            };
            _process.ErrorDataReceived += (_, e) =>
            {
                if (!string.IsNullOrWhiteSpace(e.Data))
                {
                    Console.WriteLine("[Router:ERR] " + e.Data);
                }
            };
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
        }

        public override void Stop()
        {
            if (!_process.HasExited)
            {
                try
                {
                    _process.Kill(true);
                    _process.WaitForExit(3000);
                }
                catch (Exception e)
                {
                    Console.WriteLine("Router encountered error while stopping: " + e.Message);
                }
                finally
                {
                    _process.Dispose();
                    _process = new Process();
                }
            }
            Console.WriteLine("Router shut down.");
        }
    }

    public sealed class LlmProviderQwenModule : Module
    {
        private Process _process = new Process();

        public override void Run()
        {
            string dotnetRoot = "";

            var dir = new DirectoryInfo(AppContext.BaseDirectory);
            while (dir is not null)
            {
                if (File.Exists(Path.Combine(dir.FullName, "MorpheusEngine.sln")))
                {
                    dotnetRoot = dir.FullName;
                }
                dir = dir.Parent;
            }
            if (dotnetRoot == "")
            {
                dotnetRoot = Environment.CurrentDirectory;
            }

            var projectPath = Path.Combine(dotnetRoot, "src", "MorpheusEngine.LlmProvider_qwen", "MorpheusEngine.LlmProvider_qwen.csproj");
            var psi = new ProcessStartInfo
            {
                FileName = "dotnet",
                Arguments = $"run --project \"{projectPath}\" --",
                WorkingDirectory = dotnetRoot,
                UseShellExecute = false,
                CreateNoWindow = true,
                RedirectStandardOutput = true,
                RedirectStandardError = true,
            };

            _process = Process.Start(psi) ?? throw new InvalidOperationException("Failed to start LlmProviderQwenModule process.");
            _process.OutputDataReceived += (_, e) =>
            {
                if (!string.IsNullOrWhiteSpace(e.Data))
                {
                    Console.WriteLine("[Qwen] " + e.Data);
                }
            };
            _process.ErrorDataReceived += (_, e) =>
            {
                if (!string.IsNullOrWhiteSpace(e.Data))
                {
                    Console.WriteLine("[Qwen:ERR] " + e.Data);
                }
            };
            _process.BeginOutputReadLine();
            _process.BeginErrorReadLine();
        }

        public override void Stop()
        {
            if (!_process.HasExited)
            {
                try
                {
                    _process.Kill(true);
                    _process.WaitForExit(3000);
                }
                catch (Exception e)
                {
                    Console.WriteLine("LlmProvider_qwen encountered error while stopping: " + e.Message);
                }
                finally
                {
                    _process.Dispose();
                    _process = new Process();
                }
            }
            Console.WriteLine("LlmProvider_qwen shut down.");
        }
    }
}