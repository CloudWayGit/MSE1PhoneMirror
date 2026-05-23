# Project Governance

This document describes how decisions are made for **1PhoneMirror** and
who is authorised to approve release artifacts for code signing.

## Maintainers

| Role               | Person                  | GitHub handle         |
|--------------------|-------------------------|------------------------|
| Project lead       | Simon Skotheimsvik      | @SimonSkotheimsvik     |

The project lead has final say on all technical and release decisions
and is the sole maintainer at this time. Additional maintainers may be
added by the project lead.

## Decision making

- **Code changes**: accepted through GitHub pull requests, reviewed and
  merged by the project lead.
- **Releases**: cut from `main` by the project lead via the
  `Build & Release MSI` GitHub Actions workflow (`v*.*.*` tag push).
- **Security advisories**: handled per [SECURITY.md](SECURITY.md).

## Release signing

Official `1PhoneMirror-*.msi` artifacts are currently **unsigned**.
Users should verify the SHA-256 hash published on the GitHub Release
page before installing — see [SECURITY.md](SECURITY.md#release-integrity).
