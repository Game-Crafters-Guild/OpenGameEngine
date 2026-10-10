# Chromatic Aberration package

Self-contained MIT-licensed lateral, longitudinal, and coma aberration pass.
It preserves the existing intensity, edge-offset, saturation, axial blur, and
coma controls. Lateral separation is measured from red to blue in render
pixels. Longitudinal blur follows the active camera focus distance and uses the
resolved scene depth so in-focus surfaces remain sharp.

The render pipeline and extraction path require this package to be mounted.
Disabling it in a project's Package Manager bypasses the complete effect after
that project is reopened, while retaining scene settings for a later re-enable.
