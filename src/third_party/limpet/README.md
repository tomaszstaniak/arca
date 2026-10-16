# Limpet SDK (vendored)

`limpet_query.c`, `limpet_format.c` and `include/limpet/` are copied unchanged
from the Limpet SDK 0.3.0 (aros-xterm repository, commit f3a9f59, directory
`sdk/`). MIT licence, `LICENSE.txt`.

unarc-cli uses only `limpet_query_presentation`: it tells whether its output
goes to a Limpet window, which shows text as UTF-8.
