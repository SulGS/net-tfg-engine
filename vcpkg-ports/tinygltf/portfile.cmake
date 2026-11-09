# Overlay of vcpkg's tinygltf 2.9.3 port (registered in vcpkg-configuration.json).
# The stock port downloads GitHub's v2.9.3 tarball, but GitHub now serves a different archive for that tag
# (no tools/ folder) so its SHA512 no longer matches and fresh installs fail. Fetching the tag's commit
# through git is content-addressed and can't drift. tiny_gltf.h is identical in both.

# Header-only library
vcpkg_from_git(
    OUT_SOURCE_PATH SOURCE_PATH
    URL https://github.com/syoyo/tinygltf.git
    REF 14ba27113ebd507ebeb7cba8ecaeab8df3921b27 # v2.9.3
)

# Copy the tinygltf header files and fix the path to json
vcpkg_replace_string("${SOURCE_PATH}/tiny_gltf.h" "#include \"json.hpp\"" "#include <nlohmann/json.hpp>")
file(INSTALL "${SOURCE_PATH}/tiny_gltf.h" DESTINATION "${CURRENT_PACKAGES_DIR}/include")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
