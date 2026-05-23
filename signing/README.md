# Azure Trusted Signing infrastructure

Provisions everything 1PhoneMirror needs to sign its release MSI with
[Azure Trusted Signing](https://learn.microsoft.com/azure/trusted-signing/)
from GitHub Actions, authenticated via OIDC federated credentials (no
client secrets stored in GitHub).

This is **separate** from the [telemetry/](../telemetry) Azure project —
different RG, different lifecycle, deployed with plain Az PowerShell
(no `azd`).

## Prerequisites

- **PowerShell 7+** with the `Az` module (`Install-Module Az`). The
  current scaffolding has been tested with `Az.Accounts 5.3.3` and
  `Az.Resources 9.0.3`.
- **Bicep CLI** on `PATH` — Az PowerShell shells out to `bicep` to
  transpile templates. Install with `winget install -e --id Microsoft.Bicep`
  and restart the shell.
- An identity with Contributor on the target subscription.

## What gets deployed

| Resource | Purpose |
|---|---|
| `1phonemirror-signing-rg` (RG) | Holds all signing infra. Default region `westeurope`. |
| `tsa1phonemirror` (Microsoft.CodeSigning/codeSigningAccounts) | Trusted Signing account, Basic SKU (~$10/mo). |
| `release-signing` (certificateProfiles) | Created in Phase 2 once identity validation is approved. |
| `1phonemirror-signing-ci` (UAMI) | Identity that GitHub Actions assumes via OIDC. |
| 2× federatedIdentityCredentials | One for `main` branch, one for the `release` GitHub Environment. |
| Role assignment | *Trusted Signing Certificate Profile Signer* on the account, granted to the UAMI. |

## Subscription / region

- **Subscription:** `fa164986-1339-43d7-892a-1f797d3919f9` (Visual
  Studio Enterprise – MVP). Costs are billed against the monthly MVP
  Azure credit.
- **Region:** `westeurope` (closest to Norway). Trusted Signing is only
  GA in a small list of regions — see the `@allowed` list in
  [main.bicep](main.bicep).

## Phased rollout

Identity validation is **not** something Bicep can create — it's a
manual review by Microsoft. So the rollout is in three phases.

### Status (this repo)

| Phase | State |
|---|---|
| 1 – Account + UAMI + federated creds deployed | ✅ Done (deployment `1phonemirror-signing-phase1`, 2026-05-23) |
| 2 – Identity validation submitted in portal | ⏳ Manual action required |
| 3 – Cert profile deployed with validation GUID | ⏳ Blocked on Phase 2 |
| 4 – GitHub variables + `release` environment | ⏳ Blocked on Phase 3 |

### Deployed identifiers (Phase 1)

| Output | Value |
|---|---|
| `azureClientId` (UAMI) | `516151bf-0914-4736-ae06-aaf29c8dbf55` |
| `azureTenantId` | `83472170-5be6-45bd-b4a7-464f4d12f820` |
| `azureSubscriptionId` | `fa164986-1339-43d7-892a-1f797d3919f9` |
| `trustedSigningAccountName` | `tsa1phonemirror` |
| `trustedSigningEndpoint` | `https://westeurope.codesigning.azure.net` |
| `certificateProfileName` | *(empty until Phase 3)* |

### Phase 1 — deploy the account (no cert profile yet)

```powershell
Connect-AzAccount -Tenant '83472170-5be6-45bd-b4a7-464f4d12f820' `
                  -Subscription 'fa164986-1339-43d7-892a-1f797d3919f9'

# Register the resource provider once per subscription.
Register-AzResourceProvider -ProviderNamespace 'Microsoft.CodeSigning'

# Optional: see exactly what will be created.
$raw = Get-Content signing/main.parameters.json -Raw | ConvertFrom-Json
$paramHash = @{}
foreach ($p in $raw.parameters.PSObject.Properties) { $paramHash[$p.Name] = $p.Value.value }
Get-AzSubscriptionDeploymentWhatIfResult `
  -Location westeurope `
  -TemplateFile signing/main.bicep `
  -TemplateParameterObject $paramHash

# Submit the deployment via the helper script.
./signing/deploy-phase1.ps1
```

This creates the RG, the Trusted Signing account, the managed identity,
the federated credentials, and the role assignment. No cert profile is
created because `identityValidationId` is empty.

### Phase 2 — complete identity validation in the portal

1. Open the [Trusted Signing accounts blade](https://portal.azure.com/#blade/HubsExtension/BrowseResource/resourceType/Microsoft.CodeSigning%2FcodeSigningAccounts)
   and select `tsa1phonemirror`.
2. **Identity validation → Add**.
3. Choose **Public Trust → Individual**.
4. Subject name: **`Simon Skotheimsvik`** (must match government ID
   exactly — passport / BankID).
5. Submit. Microsoft reviews within ~3 business days. Status moves from
   *Submitted* → *In Review* → *Completed*.
6. When *Completed*, copy the **Validation ID** GUID.

### Phase 3 — deploy the certificate profile

Update [main.parameters.json](main.parameters.json) → set
`identityValidationId` to the GUID from Phase 2, then re-deploy:

```powershell
./signing/deploy-phase1.ps1 -DeploymentName 1phonemirror-signing-phase3
```

This is idempotent — Phase 1 resources are unchanged; only the cert
profile is added. Capture the deployment outputs:

```powershell
(Get-AzSubscriptionDeployment -Name 1phonemirror-signing-phase3).Outputs
```

### Phase 4 — wire up GitHub

Set the following on `MSEndpointMgr/1PhoneMirror`:

**Repository variables** (Settings → Secrets and variables → Actions → Variables):

| Variable | Source |
|---|---|
| `AZURE_CLIENT_ID` | `azureClientId` output |
| `AZURE_TENANT_ID` | `azureTenantId` output |
| `AZURE_SUBSCRIPTION_ID` | `azureSubscriptionId` output |
| `AZURE_TRUSTED_SIGNING_ENDPOINT` | `trustedSigningEndpoint` output (e.g. `https://westeurope.codesigning.azure.net`) |
| `AZURE_TRUSTED_SIGNING_ACCOUNT` | `trustedSigningAccountName` output |
| `AZURE_TRUSTED_SIGNING_PROFILE` | `certificateProfileName` output |

No secrets are required — OIDC handles auth.

**GitHub Environment** (Settings → Environments → New environment):

1. Name: `release`.
2. Deployment branches and tags → **Selected branches and tags** →
   add tag pattern `v*.*.*`.
3. (Optional) Require reviewers = yourself, for a manual gate before
   any sign happens.

Once `AZURE_TRUSTED_SIGNING_ENDPOINT` is set, the release workflow
(see [.github/workflows/release.yml](../.github/workflows/release.yml))
will sign the EXE in `dist/stage/` and the final MSI automatically.
While the variable is empty the workflow publishes unsigned MSIs, same
as today.

## Cost

- Basic tier: **$9.99/mo** flat + per-signature fee that is effectively
  zero at our release cadence.
- Stop billing by deleting the `codeSigningAccount` resource; signatures
  produced previously remain valid because they are RFC 3161
  timestamped. RBAC + UAMI cost nothing.

## Teardown

```powershell
Remove-AzResourceGroup -Name '1phonemirror-signing-rg' -Force
```

This deletes the account, cert profile, UAMI and federated credentials.
Already-signed MSIs remain valid forever (timestamp persists). The
identity validation entry survives at the tenant level — re-deploy and
reuse the same Validation ID if you stand the account back up later.
