using System.Net;
using System.Text;

namespace MorpheusEngine.Tests.Unit.Helpers;

internal sealed class MockOllamaHandler : HttpMessageHandler
{
    public sealed record CapturedRequest(string Method, string Path, string Body);

    private readonly Dictionary<(string Method, string Path), Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>>> _handlers = new();

    public List<CapturedRequest> CapturedRequests { get; } = [];

    public void OnJson(string method, string path, HttpStatusCode statusCode, string jsonBody)
    {
        OnAsync(
            method,
            path,
            (_, _) => Task.FromResult(
                new HttpResponseMessage(statusCode)
                {
                    Content = new StringContent(jsonBody, Encoding.UTF8, "application/json")
                }));
    }

    public void OnAsync(string method, string path, Func<HttpRequestMessage, CancellationToken, Task<HttpResponseMessage>> handler)
    {
        if (string.IsNullOrWhiteSpace(method))
        {
            throw new ArgumentException("method must be non-empty.", nameof(method));
        }

        if (string.IsNullOrWhiteSpace(path))
        {
            throw new ArgumentException("path must be non-empty.", nameof(path));
        }

        _handlers[(method.Trim().ToUpperInvariant(), NormalizePath(path))] = handler
            ?? throw new ArgumentNullException(nameof(handler));
    }

    protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage request, CancellationToken cancellationToken)
    {
        var body = request.Content is null
            ? string.Empty
            : await request.Content.ReadAsStringAsync(cancellationToken);
        var key = (
            Method: request.Method.Method.ToUpperInvariant(),
            Path: NormalizePath(request.RequestUri?.AbsolutePath ?? "/"));
        CapturedRequests.Add(new CapturedRequest(key.Method, key.Path, body));

        if (_handlers.TryGetValue(key, out var handler))
        {
            return await handler(request, cancellationToken);
        }

        return new HttpResponseMessage(HttpStatusCode.NotFound)
        {
            Content = new StringContent(
                $"{{\"ok\":false,\"error\":\"No mock registered for {key.Method} {key.Path}.\"}}",
                Encoding.UTF8,
                "application/json")
        };
    }

    private static string NormalizePath(string path)
    {
        var trimmed = path.Trim();
        return trimmed.StartsWith("/", StringComparison.Ordinal) ? trimmed : "/" + trimmed;
    }
}
