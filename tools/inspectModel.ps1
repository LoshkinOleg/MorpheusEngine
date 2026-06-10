<#
.SYNOPSIS
    Inspects an Ollama model via the /api/show endpoint.

.DESCRIPTION
    Fetches model info and prints three useful views:
      1. The chat template (raw, with real newlines)
      2. Tokenizer-related GGUF metadata (BOS/EOS flags, vocab size, etc.)
      3. The full JSON dump (optional, with -Full)

.PARAMETER Model
    Model tag, e.g. "qwen3:4b-instruct-2507-q4_K_M".

.PARAMETER Host
    Ollama host URL. Defaults to http://localhost:11434.

.PARAMETER Full
    Also dump the full JSON response.

.EXAMPLE
    .\Show-OllamaModel.ps1 -Model "qwen3:4b-instruct-2507-q4_K_M"

.EXAMPLE
    .\Show-OllamaModel.ps1 -Model "qwen3:4b-instruct-2507-q4_K_M" -Full
#>

param(
    [Parameter(Mandatory = $true)]
    [string]$Model,

    [string]$OllamaHost = "http://localhost:11434",

    [switch]$Full
)

$ErrorActionPreference = "Stop"

$body = @{ model = $Model; verbose = $true } | ConvertTo-Json -Compress

try {
    $response = Invoke-RestMethod `
        -Uri "$OllamaHost/api/show" `
        -Method Post `
        -Body $body `
        -ContentType "application/json"
}
catch {
    Write-Error "Failed to reach Ollama at $OllamaHost. Is the server running? ($_)"
    exit 1
}

# --- 1. Template ---
Write-Host "`n=== CHAT TEMPLATE ===" -ForegroundColor Cyan
if ($response.template) {
    Write-Host $response.template
} else {
    Write-Host "(no template field returned)" -ForegroundColor DarkGray
}

# --- 2. Tokenizer metadata ---
Write-Host "`n=== TOKENIZER METADATA (BOS/EOS, vocab) ===" -ForegroundColor Cyan
if ($response.model_info) {
    $tokenKeys = $response.model_info.PSObject.Properties |
        Where-Object { $_.Name -match "token|vocab|bos|eos" } |
        Sort-Object Name

    if ($tokenKeys.Count -eq 0) {
        Write-Host "(no token-related keys in model_info)" -ForegroundColor DarkGray
    } else {
        foreach ($p in $tokenKeys) {
            $val = $p.Value
            # Truncate long arrays (vocab lists, merges) so the console stays readable
            if ($val -is [System.Array] -and $val.Count -gt 5) {
                $val = "[array of $($val.Count) items, first 3: $($val[0..2] -join ', ') ...]"
            }
            "{0,-50} {1}" -f $p.Name, $val | Write-Host
        }
    }
} else {
    Write-Host "(no model_info field returned)" -ForegroundColor DarkGray
}

# --- 3. Quick summary ---
Write-Host "`n=== SUMMARY ===" -ForegroundColor Cyan
if ($response.details) {
    "{0,-20} {1}" -f "Family:",       $response.details.family       | Write-Host
    "{0,-20} {1}" -f "Parameter size:", $response.details.parameter_size | Write-Host
    "{0,-20} {1}" -f "Quantization:",  $response.details.quantization_level | Write-Host
    "{0,-20} {1}" -f "Format:",        $response.details.format       | Write-Host
}

# --- 4. Full dump (optional) ---
if ($Full) {
    Write-Host "`n=== FULL JSON ===" -ForegroundColor Cyan
    $response | ConvertTo-Json -Depth 10
}