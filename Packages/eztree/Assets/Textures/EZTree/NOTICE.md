EZ-Tree texture assets
======================

Bark and leaf maps in this folder are custom engine-authored textures
(stylized albedo, plus derived normal, AO, and roughness) at 512².
Leaf albedo stays PNG for cutout alpha; all other maps are JPEG. They
are not the photoreal maps that ship with upstream EZ-Tree.

The Tree Generator itself is EZ-Tree 1.1.0 (MIT, Copyright (c) 2024
Daniel Greenheck):

Repository: https://github.com/dgreenheck/ez-tree.git
Upstream commit: 28c16503da2a8a6f2ccb6c070f47ff8ca13ad4f6

The full upstream MIT license is staged from the vcpkg overlay package
`ez-tree-upstream`.
