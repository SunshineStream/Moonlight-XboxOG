# Fetch only the tagged source assets; Lucide is never compiled for the Xbox.
include_guard(GLOBAL)
CPMGetPackage(lucide)
set(SUNSHINE_LUCIDE_SOURCE_DIR "${lucide_SOURCE_DIR}")
