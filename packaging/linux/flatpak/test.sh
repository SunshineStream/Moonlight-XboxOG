#!/usr/bin/env bash
set -euo pipefail

test_status=0
ctest --test-dir . --output-on-failure --no-tests=error \
  --output-junit "${PWD}/tests/test_results.xml" || test_status=$?

# Use the SDK's gcov to read the counters written by its compiler. Source paths
# are relative to the retained source tree so gcovr can read the reports on the host.
build_dir="${PWD}"
mapfile -d '' -t notes < <(find tests -name '*.gcno' -type f -print0)
test "${#notes[@]}" -gt 0
for note in "${notes[@]}"; do
  # Keep each object's reports separate so gcovr can merge coverage from shared headers.
  report_dir="${note%.gcno}.coverage"
  mkdir -p "${report_dir}"
  (
    cd "${report_dir}"
    gcov --all-blocks --branch-probabilities --branch-counts --demangled-names --hash-filenames --relative-only \
      --source-prefix "${FLATPAK_BUILDER_BUILDDIR}" "${build_dir}/${note}" > /dev/null
  )
done

exit "${test_status}"
