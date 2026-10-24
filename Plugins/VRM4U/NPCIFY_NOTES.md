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
