# Project Governance

This document describes how decisions are made for **1PhoneMirror** and
who is authorised to approve release artifacts for code signing.

## Maintainers

| Role               | Person                  | GitHub handle         |
|--------------------|-------------------------|------------------------|
| Project lead       | Simon Skotheimsvik      | @SimonSkotheimsvik     |

The project lead has final say on all technical and release decisions
and is the sole maintainer at this time. Additional maintainers may be
added by the project lead; this document and the SignPath signing
policy will be updated at the same time.

## Decision making

- **Code changes**: accepted through GitHub pull requests, reviewed and
  merged by the project lead.
- **Releases**: cut from `main` by the project lead via the
  `Build & Release MSI` GitHub Actions workflow (`v*.*.*` tag push).
- **Security advisories**: handled per [SECURITY.md](SECURITY.md).

## Release signing approvals

Official `1PhoneMirror-*.msi` artifacts are signed through
[SignPath.io](https://signpath.io)'s open-source code signing program
under the *SignPath Foundation* certificate.

Per the [SignPath signing policy](.signpath/signing-policy.md), every
signing request requires explicit human approval before SignPath issues
a signature.

| Action                                | Authorised approver(s)    |
|---------------------------------------|----------------------------|
| Approve a `release` signing request   | Simon Skotheimsvik (sole)  |
| Submit a signing request from CI      | The `Build & Release MSI` workflow on the `main` branch of `MSEndpointMgr/1PhoneMirror`, on push of a `v*.*.*` tag |
| Submit a signing request locally      | Simon Skotheimsvik via `package.ps1 -SubmitForSigning` |

The approver must:

1. Verify the workflow run was triggered by a `v*.*.*` tag push on
   `main` of `MSEndpointMgr/1PhoneMirror`.
2. Verify the source commit SHA shown in the SignPath request matches
   the tag in the public repository.
3. Verify the artifact filename matches `1PhoneMirror-<version>.msi`
   where `<version>` matches the tag.
4. Spot-check the `dist/stage` manifest in the workflow logs for any
   unexpected binaries.

If any check fails, the approver must **reject** the signing request
and investigate before retrying.

## Adding a new approver

To add a second approver in the future, the project lead must:

1. Open a PR updating this file and `.signpath/signing-policy.md`.
2. Mirror the change in the SignPath project's "Submitters" /
   "Approvers" lists in the SignPath dashboard.
3. Update the `SECURITY.md` contact list if appropriate.

Until that PR is merged and the SignPath dashboard reflects the
change, the sole approver remains Simon Skotheimsvik.
