// =============================================================================
// 1PhoneMirror — Azure Trusted Signing infrastructure (subscription scope)
// =============================================================================
//
// Provisions everything needed to sign 1PhoneMirror release artifacts with
// Azure Trusted Signing from GitHub Actions, using federated identity
// (OIDC) — no client secrets stored in GitHub.
//
// Resources created:
//   - Resource group (1phonemirror-signing-rg by default)
//   - Microsoft.CodeSigning/codeSigningAccounts (Basic SKU)
//   - User-assigned managed identity (signed in via GitHub OIDC)
//   - Two federated identity credentials on that UAMI:
//       * main branch (manual workflow_dispatch)
//       * "release" GitHub Environment (tag-triggered releases)
//   - Role assignment: "Trusted Signing Certificate Profile Signer" on the
//     code-signing account for the UAMI
//   - (Optional, Phase 2) Microsoft.CodeSigning/.../certificateProfiles
//     — created only after identity validation completes (see README).
//
// Deploy with:
//   az deployment sub create `
//     --location westeurope `
//     --template-file signing/main.bicep `
//     --parameters signing/main.parameters.json
//
// See signing/README.md for the full multi-phase rollout.
// =============================================================================

targetScope = 'subscription'

@description('Azure region for Trusted Signing. Must be one of the supported regions: westeurope, northeurope, eastus, westcentralus, westus3.')
@allowed([
  'westeurope'
  'northeurope'
  'eastus'
  'westcentralus'
  'westus3'
])
param location string = 'westeurope'

@description('Name of the resource group that holds the signing infrastructure.')
param resourceGroupName string = '1phonemirror-signing-rg'

@description('Trusted Signing account name (3-24 chars, alnum + hyphens, must start with a letter).')
@minLength(3)
@maxLength(24)
param codeSigningAccountName string = 'tsa1phonemirror'

@description('Certificate profile name (5-100 chars, alnum + hyphens, must start with a letter).')
@minLength(5)
@maxLength(100)
param certificateProfileName string = 'release-signing'

@description('GitHub owner/repo that is allowed to obtain federated tokens for this identity.')
param githubRepository string = 'MSEndpointMgr/1PhoneMirror'

@description('Name of the GitHub Environment used to gate production releases. Configure deployment branch/tag policy in GitHub UI.')
param githubReleaseEnvironment string = 'release'

@description('Identity validation GUID issued by Microsoft after Individual/Organization verification completes. Leave empty during Phase 1 (account-only) deployment; provide it in Phase 2 to create the certificate profile.')
param identityValidationId string = ''

@description('Common resource tags.')
param tags object = {
  project: '1PhoneMirror'
  purpose: 'code-signing'
  managedBy: 'bicep'
}

// -----------------------------------------------------------------------------
// Resource group
// -----------------------------------------------------------------------------
resource rg 'Microsoft.Resources/resourceGroups@2024-03-01' = {
  name: resourceGroupName
  location: location
  tags: tags
}

// -----------------------------------------------------------------------------
// Resources inside the RG (delegated to module)
// -----------------------------------------------------------------------------
module signing 'resources.bicep' = {
  name: 'signing-resources'
  scope: rg
  params: {
    location: location
    codeSigningAccountName: codeSigningAccountName
    certificateProfileName: certificateProfileName
    githubRepository: githubRepository
    githubReleaseEnvironment: githubReleaseEnvironment
    identityValidationId: identityValidationId
    tags: tags
  }
}

// -----------------------------------------------------------------------------
// Outputs (used by the release workflow)
// -----------------------------------------------------------------------------
@description('Trusted Signing endpoint URL — set as AZURE_TRUSTED_SIGNING_ENDPOINT in GitHub Actions.')
output trustedSigningEndpoint string = signing.outputs.trustedSigningEndpoint

@description('Trusted Signing account name — set as AZURE_TRUSTED_SIGNING_ACCOUNT.')
output trustedSigningAccountName string = signing.outputs.codeSigningAccountName

@description('Certificate profile name — set as AZURE_TRUSTED_SIGNING_PROFILE (empty until Phase 2).')
output certificateProfileName string = signing.outputs.certificateProfileName

@description('Client ID of the user-assigned managed identity — set as AZURE_CLIENT_ID.')
output azureClientId string = signing.outputs.managedIdentityClientId

@description('Tenant ID — set as AZURE_TENANT_ID.')
output azureTenantId string = subscription().tenantId

@description('Subscription ID — set as AZURE_SUBSCRIPTION_ID.')
output azureSubscriptionId string = subscription().subscriptionId
