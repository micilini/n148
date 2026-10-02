# Original public V1 baseline

`n148i-v1-source.tar.gz` contains the unmodified library source and build files
from the first public library merge, commit
`86cd4feb285c24ae26106bf6b3811194a8c149c2` (6 September 2026).
The original MIT license is included in the archive.

[provenance.json](provenance.json) records the archive digest and the SHA-256
of all 37 files, verified against that commit before the release history was
consolidated. This source archive is retained so the comparison can be
reproduced without the old Git history.

The release preparation tool builds V1 separately with `-O3`. The common
measurement driver uses V1's original header and smaller options structure.
It does not emulate V1 using the current encoder or change the baseline code.
Historical version strings inside this archive identify the baseline; the
active product and library are **N.148i V2**.
