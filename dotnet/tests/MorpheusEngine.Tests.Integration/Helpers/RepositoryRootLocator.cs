namespace MorpheusEngine.Tests.Integration.Helpers;

// Resolves the live repository root from the integration test assembly's compiled output
// directory. The harnesses use this to copy real schema / lore / system-prompt fixtures into
// per-test temp project trees so the modules under test load assets that match production.
//
// The walk is fixed at six parent levels because the integration test DLL is laid out as:
//   <repo>/dotnet/tests/MorpheusEngine.Tests.Integration/bin/<Configuration>/net9.0/<dll>
//   level: 6   5      4                                  3   2              1     0
// Anything that breaks that layout will break this helper loudly via the missing-file errors
// downstream, which is preferable to silently reading from a wrong directory.
internal static class RepositoryRootLocator
{
    private const int LEVELS_FROM_TEST_BIN_TO_REPO_ROOT = 6;

    public static string GetRepositoryRoot()
    {
        var segments = new string[LEVELS_FROM_TEST_BIN_TO_REPO_ROOT + 1];
        segments[0] = AppContext.BaseDirectory;
        for (var i = 1; i <= LEVELS_FROM_TEST_BIN_TO_REPO_ROOT; i++)
        {
            segments[i] = "..";
        }

        return Path.GetFullPath(Path.Combine(segments));
    }
}
