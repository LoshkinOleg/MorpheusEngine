namespace MorpheusEngine;

/// <summary>
/// Child-process contract for host-driven run binding. The host invokes this once per process via
/// POST /initialize (loopback-only in module implementations).
/// </summary>
public interface IEngineRunBinder
{
    bool IsRunBound { get; }

    Task BindRunAsync(InitializeModuleRequest request, CancellationToken cancellationToken);
}

