# CPM Package Lock
# This file should be committed to version control.
# Renovate reads the metadata comments using LizardByte's inherited CMake preset.
# Keep tracked VERSION/TAG values immediately below their renovate metadata.

# Lucide SVG icons
# renovate: datasource=github-tags depName=lucide-icons/lucide
set(LUCIDE_VERSION 1.75.0)
CPMDeclarePackage(lucide
        NAME lucide
        VERSION ${LUCIDE_VERSION}
        GIT_REPOSITORY https://github.com/lucide-icons/lucide.git
        GIT_TAG ${LUCIDE_VERSION}
        DOWNLOAD_ONLY YES
)
