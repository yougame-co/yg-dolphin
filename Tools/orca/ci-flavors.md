# Manual CI builds: flavors and release intake

The three manual workflows (`.github/workflows/linux-x64-build.yml`, `macos-build.yml`, `windows-x64-build.yml`) take
`version`, `commit` (the public snapshot to build), `source_commit` (the private release commit it matches) and `flavor`.

| flavor  | flat-bundle public key            | ORCA_STAGING_BUILD | artifact names                                  |
|---------|-----------------------------------|--------------------|-------------------------------------------------|
| staging | STAGING `8422b8b2...`             | `1`                | `yg-dolphin-<v>-<commit>-<platform>-staging-*`  |
| prod    | PRODUCTION `3b658bd5...`          | unset              | `yg-dolphin-<v>-<commit>-<platform>-prod-*`     |

Every flavor refuses the TEST key. A prod build also refuses the STAGING key, as the pinned key and inside the program
(the packaging scripts and `Tools/orca/bundle-key-guard.py`; the macOS and Windows check steps grep the program too).
The run title carries all four inputs, so a run can be matched to a release by its title alone.

## What the release intake needs from main

YouGame's intake verifiers take a CI run only when the workflow commit it ran on entered `main` through a reviewed PR:

- an approving GitHub review of the PR's exact head, or a reviewer's recorded approval of that head written before the
  merge (`orca-approvals/yg-dolphin-pr<N>-<head8>.approval.json`, with its review file and the review's mailbox number);
- for a recorded approval, the merge commit's message names that mailbox number, e.g.
  `Reviewed and approved by <reviewer> (mailbox #<n>; approval file orca-approvals/<file> sha256 <digest>)`.
  Write it in the merge body when you merge (`gh pr merge --merge --body "..."`); a default merge message is refused
  and cannot be fixed afterwards, since main is never rewritten;
- the workflow file's blob at that commit is the one the verifier pins, and the run was dispatched on `main`.
