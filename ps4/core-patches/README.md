# Per-core patches

`ps4/build-cores.sh` applies every `*.patch` in `<core>/` to that core's clone, in filename
order, after resetting the clone to the recipe's upstream branch. So a build is a function of
(upstream ref, this directory) and nothing else, and the manifest records both — a core built
with patches shows `<commit>+<n>`.

    ps4/core-patches/picodrive/0001-something.patch

## Why patches and not edited clones

The clones live outside this repository and `--update` throws local edits away. A fix that
matters has to survive that, be reviewable, and travel with the port. `git format-patch` or
`git diff > file.patch` from inside the clone both produce something this applies.

## ⚠ What belongs here and what does not

A patch is for making a core **build or behave** on this platform: a platform arm, a header that
this SDK spells differently, an assumption about `dlopen`. It is not the place to work around a
harness bug — two of the first sweep's failures were the harness searching the wrong directory
for objects and a `grep -q` under `pipefail` killing `llvm-nm` with SIGPIPE, and patching cores
would have buried both.

Send anything that is not PS4-specific upstream instead. A patch here is a cost that has to be
carried at every `--update`.
