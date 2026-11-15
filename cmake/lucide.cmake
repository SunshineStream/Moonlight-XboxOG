# Fetch only the tagged source assets; Lucide is never compiled for the Xbox.
include_guard(GLOBAL)
CPMGetPackage(lucide)
set(MOONLIGHT_LUCIDE_SOURCE_DIR "${lucide_SOURCE_DIR}")
