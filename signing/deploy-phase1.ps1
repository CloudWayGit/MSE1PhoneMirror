#!/usr/bin/env pwsh
# Phase 1 deployment helper for Azure Trusted Signing.
# Reads signing/main.parameters.json and submits a subscription-scope
# deployment with identityValidationId left empty.
[CmdletBinding()]
param(
  [string]$DeploymentName = '1phonemirror-signing-phase1',
  [string]$Location = 'westeurope'
)

$ErrorActionPreference = 'Stop'
$scriptDir = Split-Path -Parent $MyInvocation.MyCommand.Path

$raw = Get-Content (Join-Path $scriptDir 'main.parameters.json') -Raw | ConvertFrom-Json
$paramHash = @{}
foreach ($p in $raw.parameters.PSObject.Properties) {
  $paramHash[$p.Name] = $p.Value.value
}

Write-Host "Deployment parameters:" -ForegroundColor Cyan
$paramHash.GetEnumerator() | Sort-Object Name | Format-Table -AutoSize | Out-String | Write-Host

Write-Host "Submitting deployment '$DeploymentName' to '$Location'..." -ForegroundColor Cyan
New-AzSubscriptionDeployment `
  -Name $DeploymentName `
  -Location $Location `
  -TemplateFile (Join-Path $scriptDir 'main.bicep') `
  -TemplateParameterObject $paramHash
