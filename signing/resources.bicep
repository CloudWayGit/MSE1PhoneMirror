// =============================================================================
// Resource-group-scoped Trusted Signing resources for 1PhoneMirror.
// Invoked as a module from main.bicep — do not deploy directly.
// =============================================================================

targetScope = 'resourceGroup'

param location string
param codeSigningAccountName string
param certificateProfileName string
param githubRepository string
param githubReleaseEnvironment string
param identityValidationId string
param tags object

// -----------------------------------------------------------------------------
// User-assigned managed identity for GitHub Actions OIDC.
// -----------------------------------------------------------------------------
resource uami 'Microsoft.ManagedIdentity/userAssignedIdentities@2023-01-31' = {
  name: '1phonemirror-signing-ci'
  location: location
  tags: tags
}

// Federated credential 1: main branch (for workflow_dispatch test runs).
resource fedCredMain 'Microsoft.ManagedIdentity/userAssignedIdentities/federatedIdentityCredentials@2023-01-31' = {
  parent: uami
  name: 'github-main-branch'
  properties: {
    issuer: 'https://token.actions.githubusercontent.com'
    audiences: [ 'api://AzureADTokenExchange' ]
    subject: 'repo:${githubRepository}:ref:refs/heads/main'
  }
}

// Federated credential 2: GitHub Environment (tag-gated production releases).
// Configure deployment branches/tags = "v*" in the GitHub Environment settings.
resource fedCredEnv 'Microsoft.ManagedIdentity/userAssignedIdentities/federatedIdentityCredentials@2023-01-31' = {
  parent: uami
  name: 'github-release-environment'
  properties: {
    issuer: 'https://token.actions.githubusercontent.com'
    audiences: [ 'api://AzureADTokenExchange' ]
    subject: 'repo:${githubRepository}:environment:${githubReleaseEnvironment}'
  }
  dependsOn: [ fedCredMain ] // serialise to avoid 409 on parent UAMI
}

// -----------------------------------------------------------------------------
// Trusted Signing account.
// -----------------------------------------------------------------------------
resource codeSigningAccount 'Microsoft.CodeSigning/codeSigningAccounts@2024-09-30-preview' = {
  name: codeSigningAccountName
  location: location
  tags: tags
  properties: {
    sku: {
      name: 'Basic'
    }
  }
}

// -----------------------------------------------------------------------------
// Certificate profile (Phase 2 — requires identityValidationId).
//
// Identity validation is a manual step in the Azure portal:
//   Trusted Signing account → Identity validation → Add → Individual.
// After Microsoft approves it (~3 business days for Individual), grab the
// validation GUID and pass it as -identityValidationId on the next deploy.
// -----------------------------------------------------------------------------
resource certProfile 'Microsoft.CodeSigning/codeSigningAccounts/certificateProfiles@2024-09-30-preview' = if (!empty(identityValidationId)) {
  parent: codeSigningAccount
  name: certificateProfileName
  properties: {
    profileType: 'PublicTrust'
    identityValidationId: identityValidationId
  }
}

// -----------------------------------------------------------------------------
// RBAC: grant the UAMI permission to sign with this account.
// Built-in role: "Trusted Signing Certificate Profile Signer".
// See: https://learn.microsoft.com/en-us/azure/trusted-signing/concept-trusted-signing-resources-roles
// -----------------------------------------------------------------------------
var trustedSigningCertificateProfileSignerRoleId = '2837e146-70d7-4cfd-ad55-7efa6464f958'

resource signerRoleAssignment 'Microsoft.Authorization/roleAssignments@2022-04-01' = {
  scope: codeSigningAccount
  name: guid(codeSigningAccount.id, uami.id, trustedSigningCertificateProfileSignerRoleId)
  properties: {
    roleDefinitionId: subscriptionResourceId('Microsoft.Authorization/roleDefinitions', trustedSigningCertificateProfileSignerRoleId)
    principalId: uami.properties.principalId
    principalType: 'ServicePrincipal'
  }
}

// -----------------------------------------------------------------------------
// Outputs
// -----------------------------------------------------------------------------
output codeSigningAccountName string = codeSigningAccount.name
output certificateProfileName string = empty(identityValidationId) ? '' : certificateProfileName
output trustedSigningEndpoint string = 'https://${location}.codesigning.azure.net'
output managedIdentityClientId string = uami.properties.clientId
output managedIdentityPrincipalId string = uami.properties.principalId
