# VRM4U in this demo

This folder is VRM4U `v1.2026.10.17` for UE 5.8
(<https://github.com/ruyo/VRM4U>, MIT license), with these local changes:

- `VRM4U.uplugin`: `"Installed": false`, so that UnrealBuildTool compiles the
  plugin from source on every platform. The prebuilt Windows binaries are removed.
- `ThirdParty/assimp/lib/Mac/libassimp.a`: universal (arm64 and x86_64) static
  library, built from <https://github.com/ruyo/assimp> (`master`, glTF importer only):

  ```bash
  cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
    -DASSIMP_BUILD_TESTS=OFF -DASSIMP_BUILD_ASSIMP_TOOLS=OFF -DASSIMP_BUILD_ZLIB=OFF \
    -DASSIMP_NO_EXPORT=ON -DASSIMP_BUILD_ALL_IMPORTERS_BY_DEFAULT=OFF \
    -DASSIMP_BUILD_GLTF_IMPORTER=ON -DASSIMP_WARNINGS_AS_ERRORS=OFF \
    -DCMAKE_OSX_ARCHITECTURES="arm64;x86_64" -DCMAKE_OSX_DEPLOYMENT_TARGET=13.0
  cmake --build build -j
  ```

- `Source/VRM4ULoader/VRM4ULoader.Build.cs`: links the system zlib on macOS,
  because the library above uses it.
- `Source/VRM4ULoader/Private/VrmAsyncLoadAction.cpp`: renames a lambda parameter
  that shadows a member (clang `-Wshadow` is an error in UE 5.8).
- `Source/VRM4ULoader/Private/LoaderBPFunctionLibrary.cpp`: skips the Content
  Browser refresh after an import when Slate is not running, so that the
  `NpcifyImportCharacters` commandlet can import without the editor window.

## Node constraints (VRMC_node_constraint)

VRM4U `v1.2026.10.17` imports no node constraint of a VRM 1.0 file: `VrmConvertMetadata.cpp` looks for
`VRMC_node_constraint` at the document's root, not in each node (upstream pull request ruyo/VRM4U#598). Its
constraint node also walks the constraints in the map's order, so a constrained bone under a constrained bone needs
its parent first. The VRM4U sources here are not changed. Instead, the `NpcifyImportCharacters` commandlet fills the
rotation constraints from the file's nodes into the character's meta object after the import, parents first, and
saves it. The log line `Rotation constraints: N` gives their number.

Measured on a generated character with a long skirt (12 rotation constraints in 6 parent-child pairs between the
thighs, standing idles, 600 frames each): every constraint within 0.002 degrees of the expected rotation, also after
the editor is started again. Without the fill the helper bones did not turn, and the skirt's front panel moved up to
17 cm behind the knees.
