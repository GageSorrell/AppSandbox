# Beads on feat/custom-resolution

This branch uses Beads with the AppSandbox issue prefix. The embedded Dolt
database is local and ignored by Git. Sync uses the same GitHub repository as
the Git remote named Origin:

    git+ssh://git@github.com/GageSorrell/AppSandbox.git

Issue history is stored in the repository's separate `refs/dolt/data` ref.
That ref belongs to the repository, rather than a source branch. The setup
files and automatic hooks are scoped to `feat/custom-resolution`; no other
source branch is modified or automatically synced.

## Hook installation

The current checkout has this local Git configuration:

    git config --local includeIf.onbranch:feat/custom-resolution.path ../.beads/hooks.gitconfig

The conditional include activates `.beads/hooks` only on this branch. There
were no existing active hooks or core.hooksPath to preserve at installation.
In another clone, first install Beads and Dolt, run `bd bootstrap`, and then
run the command above from the repository root. Confirm the remote with
`bd dolt remote list`; if absent, run:

    bd dolt remote add origin git+ssh://git@github.com/GageSorrell/AppSandbox.git --allow-git-origin

## Automatic synchronization

- `pre-push` runs `bd dolt push` when pushing this branch. Failure blocks the
  Git push. Dolt-only ref pushes are skipped to avoid recursive syncing.
- `post-merge` runs `bd dolt pull` after a successful merge, including a
  fast-forward pull. It also runs for direct merges.
- `post-rewrite` runs `bd dolt pull` after a rebase, including rebase pulls.
  Amended commits are skipped.

Git has no pre-pull or post-pull hook. A pull that is already up to date does
not trigger these hooks. Use `bd dolt pull` explicitly in that case. A failed
post hook reports the error but cannot undo the completed Git merge/rebase.
Detached HEADs and other branches do not run automatic Beads synchronization.

Manual synchronization:

    bd dolt push
    bd dolt pull
