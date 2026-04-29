using System.Net.Http;
using System.Text;
using System.Text.Json;

namespace MorpheusEngine;

public sealed record RouterProxyResponse<TResponse>(
    int StatusCode,
    string RawBody,
    TResponse? Payload,
    string? DeserializeError);

/// <summary>
/// Convenience client for calling other modules through the Router's POST /proxy endpoint.
/// Keeps the wire protocol (ModuleProxyRequest) centralized so module handlers stay readable.
/// </summary>
public sealed class RouterProxyClient
{
    private readonly HttpClient _httpClient;
    private readonly int _routerPort;
    private readonly string _sourceModule;
    private readonly JsonSerializerOptions _jsonOptions;

    public RouterProxyClient(
        HttpClient httpClient,
        EngineConfiguration configuration,
        string sourceModule,
        JsonSerializerOptions jsonOptions)
    {
        _httpClient = httpClient ?? throw new ArgumentNullException(nameof(httpClient));
        _ = configuration ?? throw new ArgumentNullException(nameof(configuration));
        _sourceModule = string.IsNullOrWhiteSpace(sourceModule)
            ? throw new ArgumentException("sourceModule must be non-empty.", nameof(sourceModule))
            : sourceModule.Trim();
        _jsonOptions = jsonOptions ?? throw new ArgumentNullException(nameof(jsonOptions));
        _routerPort = configuration.GetRequiredListenPort("router");
    }

    public async Task<RouterProxyResponse<TResponse>> PostAsync<TRequest, TResponse>(
        string targetModule,
        string targetPath,
        TRequest request,
        CancellationToken cancellationToken = default)
    {
        if (string.IsNullOrWhiteSpace(targetModule))
        {
            throw new ArgumentException("targetModule must be non-empty.", nameof(targetModule));
        }

        if (string.IsNullOrWhiteSpace(targetPath))
        {
            throw new ArgumentException("targetPath must be non-empty.", nameof(targetPath));
        }

        var proxyRequest = new ModuleProxyRequest(
            _sourceModule,
            targetModule.Trim(),
            EngineConfiguration.NormalizePath(targetPath),
            "POST",
            JsonSerializer.SerializeToElement(request, _jsonOptions));

        var json = JsonSerializer.Serialize(proxyRequest, _jsonOptions);
        using var httpRequest = new HttpRequestMessage(
            HttpMethod.Post,
            $"http://127.0.0.1:{_routerPort}{EngineConfiguration.NormalizePath("/proxy")}")
        {
            Content = new StringContent(json, Encoding.UTF8, "application/json")
        };

        HttpResponseMessage response;
        try
        {
            response = await _httpClient.SendAsync(httpRequest, cancellationToken);
        }
        catch (Exception e)
        {
            throw new InvalidOperationException(
                $"Router proxy request failed for {_sourceModule} -> {targetModule.Trim()} POST {EngineConfiguration.NormalizePath(targetPath)}: {e.Message}",
                e);
        }

        using (response)
        {
            var body = await response.Content.ReadAsStringAsync(cancellationToken);
            var statusCode = (int)response.StatusCode;
            if (!response.IsSuccessStatusCode)
            {
                return new RouterProxyResponse<TResponse>(statusCode, body, default, null);
            }

            try
            {
                var payload = JsonSerializer.Deserialize<TResponse>(body, _jsonOptions);
                return new RouterProxyResponse<TResponse>(statusCode, body, payload, null);
            }
            catch (JsonException e)
            {
                return new RouterProxyResponse<TResponse>(statusCode, body, default, e.Message);
            }
        }
    }
}

