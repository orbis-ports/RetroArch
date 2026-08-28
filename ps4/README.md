# The PlayStation 4 port

Everything in this directory is read by the build or by a script. **The record is not here.**

    ps4-mesa-docs   docs/retroarch/HANDOFF.md     the running account of this port
                    docs/retroarch/PLAN.md        the plan it executed
                    docs/retroarch/CORE-CONTENT.md what each built core needs to run

That repository is where the .md goes from now on. A code repository stays a code repository:
over half this branch's history was once commits that touched nothing but a Markdown file.

## ⚠ Two Markdown files stayed, and they are not documentation

They are inputs, and CI checks out only this repository:

    RELEASE-NOTES.md   .github/workflows/frontend.yml passes it as `body_path`, so it is the text
                       of every GitHub Release. Moving it would make cutting a release depend on
                       a second checkout.
    CORE-STATUS.md     shard-cores.sh reads its FIRST table as the size proxy that deals 164
                       cores into balanced shards - see the comment there, which also explains
                       why it reads only the first. A data table that happens to read as prose.

`core-patches/README.md` stayed as well: it is the README of a directory of patches, read by
whoever is editing them, beside what it describes.
