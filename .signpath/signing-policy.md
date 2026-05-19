# SignPath Signing Policy

This file documents how 1PhoneMirror uses the
[SignPath.io](https://signpath.io) open-source code signing program. It
mirrors what is configured in the SignPath dashboard for the
`1PhoneMirror` project under the **MSEndpointMgr** organisation and is
kept in version control so changes are visible and auditable.

The authoritative source is the SignPath dashboard; this file documents
intent and the human side of the workflow.

## Project

| Field              | Value                                            |
|--------------------|--------------------------------------------------|
| SignPath org       | MSEndpointMgr                                    |
| SignPath project   | `1PhoneMirror`                                   |
| Public repository  | <https://github.com/MSEndpointMgr/1PhoneMirror>  |
| License            | GPL-3.0-or-later                                 |
| Artifact pattern   | `1PhoneMirror-<version>.msi`                     |
| Certificate        | SignPath Foundation (OV, OSS program)            |

## Signing policies

Two policies are configured:

### `release` (production)

- **Used for**: GitHub Releases attached to `v*.*.*` tags on the `main`
  branch of `MSEndpointMgr/1PhoneMirror`.
- **Submitters**:
  - The `Build & Release MSI` GitHub Actions workflow on
    `MSEndpointMgr/1PhoneMirror` (`main` branch, `v*.*.*` tag push).
  - The maintainer running `package.ps1 -SubmitForSigning` locally.
- **Approvers**: Simon Skotheimsvik (sole approver — see
  [GOVERNANCE.md](../GOVERNANCE.md)).
- **Approval mode**: Manual. Every signing request requires explicit
  human approval in the SignPath dashboard before a signature is issued.
- **Timestamping**: RFC 3161, SHA-256, with a SignPath-managed
  timestamp authority.
- **Signing method**: Authenticode for MSI; SHA-256 file digest.

### `test` (optional, dry-run)

- **Used for**: maintainer dry-runs from a non-tagged build.
- **Submitters**: Simon Skotheimsvik only.
- **Approvers**: Simon Skotheimsvik only.
- **Certificate**: SignPath test certificate (not trusted by Windows).

> Pre-release / `workflow_dispatch` builds should use the `test` policy
> so accidental production signatures cannot be issued from untagged
> commits.

## What gets signed

- The final `dist/1PhoneMirror-<version>.msi` produced by `package.ps1`.
- The `1PhoneMirror.exe` *inside* the MSI is **not** independently
  submitted — it is signed transitively when the MSI is signed
  (Authenticode covers the embedded streams).

If a future release distributes additional standalone binaries (for
example a CLI helper), they will be added to the project's artifact
configuration and this file will be updated in the same PR.

## Approver checklist

See [GOVERNANCE.md → Release signing approvals](../GOVERNANCE.md#release-signing-approvals).

## Reporting policy violations

If you believe a signed `1PhoneMirror-*.msi` has been published without
following this policy (for example, signed from a fork or from a
non-tagged commit), please report it through the channels in
[SECURITY.md](../SECURITY.md). The maintainer will investigate, revoke
the affected signature via SignPath if needed, and publish an advisory.
