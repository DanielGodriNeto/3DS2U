#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace arm11 {

// Final files of one unit set. Generated text is first staged at TempPathFor(final).
struct UnitSetFiles {
    std::string main_path;
    std::string chunk_stem;                                  // main file name without ".cpp"
    std::vector<std::string> chunk_paths;                    // unit i -> <stem>_blocks_NNNN.cpp
    std::vector<std::vector<std::uint32_t>> expected_keys;   // block keys registered by unit i, in order
};

// Staging name of a final file: the final path plus ".3ds2u-tmp".
std::string TempPathFor(const std::string& final_path);

// Removes staged files that are still present when the run ends, so a failed run leaves no staged output.
class TempFileSet {
public:
    TempFileSet() = default;
    TempFileSet(const TempFileSet&) = delete;
    TempFileSet& operator=(const TempFileSet&) = delete;
    ~TempFileSet();

    // Records final_path and returns the staging path the caller writes to.
    std::string Track(const std::string& final_path);

private:
    std::vector<std::string> staged_;
};

// Reads every staged file back. Chunk N must register exactly expected_keys[N];
// the main file must register nothing itself and call units 0..N-1 in order.
// Throws std::runtime_error on any mismatch.
void VerifyStagedFiles(const UnitSetFiles& files);

// Publishes staged files over the finals. Refuses, before changing anything, when a final
// or a stale match is not a regular file. Identical content keeps the existing file untouched.
// Stale matches are regular files named exactly <stem>_blocks_NNNN.cpp that are not in files.
// Renames chunks first, then main; if a rename fails, the finals renamed in this run are removed.
// Stale removal is not undone. Throws std::runtime_error on refusal or failure.
void PublishStagedFiles(const UnitSetFiles& files);

} // namespace arm11
