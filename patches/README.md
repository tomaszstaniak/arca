# Project patches

One directory per modified external repository, named by its ID in
`../upstreams.json`. Each holds a `series` file (application order, one patch file
name per line) and the numbered patches. Each patch is meant to be sent upstream on
its own; the program never depends on one being accepted.

Commands and conventions: [patching guide](../docs/patching.md).
