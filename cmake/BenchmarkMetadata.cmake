# Called only after linking the optional benchmark DLL; never hashes document data.
file(READ "${TEMPLATE}" metadata)
file(SHA256 "${DLL}" dll_sha256)
execute_process(COMMAND "${GIT}" -C "${ROOT}" rev-parse HEAD
    RESULT_VARIABLE revision_result OUTPUT_VARIABLE revision OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND "${GIT}" -C "${ROOT}" status --porcelain --untracked-files=normal
    RESULT_VARIABLE status_result OUTPUT_VARIABLE dirty OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT revision_result EQUAL 0 OR NOT status_result EQUAL 0)
    message(FATAL_ERROR "Benchmark build provenance unavailable")
endif()
# Explicit code/config allowlist: no documents, fixtures-on-disk, logs or editor files.
file(GLOB code_files RELATIVE "${ROOT}"
    "${ROOT}/include/*.hpp" "${ROOT}/src/*.cpp" "${ROOT}/src/*.hpp"
    "${ROOT}/tests/support/NativeBenchmark*.hpp")
list(APPEND code_files "tests/NativeUiBenchmark.cpp" "tests/NativeUiBenchmark.def"
    "CMakeLists.txt" "cmake/Baseline.cmake" "cmake/BenchmarkMetadata.cmake")
list(SORT code_files)
set(source_index "")
foreach(relative IN LISTS code_files)
    file(SHA256 "${ROOT}/${relative}" source_sha256)
    string(APPEND source_index "${relative}:${source_sha256}\n")
endforeach()
string(SHA256 source_digest "${source_index}")
if(dirty STREQUAL "")
    set(modified false)
else()
    set(modified true)
endif()
string(JSON metadata SET "${metadata}" dll_sha256 "\"${dll_sha256}\"")
string(JSON metadata SET "${metadata}" source_revision "\"${revision}\"")
string(JSON metadata SET "${metadata}" source_modified "${modified}")
string(JSON metadata SET "${metadata}" source_digest "\"${source_digest}\"")
file(WRITE "${OUTPUT}.tmp" "${metadata}\n")
file(RENAME "${OUTPUT}.tmp" "${OUTPUT}")
