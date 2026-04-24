namespace MorpheusEngine;

/// <summary>
/// Child-process contract for host-driven run binding. The host invokes this once per process via
/// an internal, loopback-only IPC endpoint (see <see cref="EngineInternalRoutes.BindRunPath"/>).
/// </summary>
public interface IEngineRunBinder
{
    bool IsRunBound { get; }

    Task BindRunAsync(InitializeModuleRequest request, CancellationToken cancellationToken);
}

