# Tests/Scenes/ — manually-runnable benchmark / stress reference scenes.
#
# Each subdirectory is a self-contained editor project (open with
# `--project Tests/Scenes/<Name>`) used to measure GPU-driven rendering
# performance against known scenes. Source assets are fetched on demand from
# the Khronos glTF-Sample-Assets repo via opt-in custom targets so the engine
# repo stays small and fresh clones don't trigger multi-megabyte downloads.

add_subdirectory(Sponza)
add_subdirectory(ABeautifulGame)
