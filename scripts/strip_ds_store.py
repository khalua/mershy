# PlatformIO pre-build script: deletes Finder .DS_Store files from
# managed_components/ and components/. The IDF component manager hashes every
# file in a managed component, so a .DS_Store dropped there by browsing in
# Finder fails the build with "components ... were modified on the disk".
import os

Import("env")  # noqa: F821 -- injected by PlatformIO

project_dir = env.subst("$PROJECT_DIR")  # noqa: F821
for sub in ("managed_components", "components"):
    for root, _dirs, files in os.walk(os.path.join(project_dir, sub)):
        if ".DS_Store" in files:
            os.remove(os.path.join(root, ".DS_Store"))
