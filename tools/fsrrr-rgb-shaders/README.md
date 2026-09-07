# Pre-Fog RGB shader validation

The project compiles the owned fullscreen RGB shader with native Windows FXC
and runs this validator before compiling OptiScaler. Both generated arrays are
reflected directly: VS/PS 5.0, sample-frequency PS execution, and the exact PS
input/output/resource signature are required. The actual VS output and PS input
must also match by semantic, register, mask, type, precision and stream: matching
semantic names alone did not prevent the observed DXBC o1-to-v0 linkage failure.
A failed check stops the build.

This avoids Wine versions whose HLSL compiler rejects `sample` or whose
`IsSampleFrequencyShader` implementation always returns false. The runtime uses
the same immutable arrays, not an unverified runtime-compiled replacement.

This check does not establish GPU pixel coverage, native alpha preservation,
or restoration of the game's bindings. Those require the live identity control.
