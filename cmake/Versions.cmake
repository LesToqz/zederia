# Pinned dependency versions (looked up 2026-11-10). Change only with a recorded reason.
set(ZED_JUCE_TAG            "9.0.5")
set(ZED_CATCH2_TAG          "v3.16.1")
set(ZED_NLOHMANN_JSON_TAG   "v3.12.0")
set(ZED_CPP_HTTPLIB_TAG     "v0.65.0")
# CLAP route: JUCE 9.0.5 has no native CLAP client, so use the community extensions (no releases; pinned commit).
set(ZED_CLAP_JUCE_EXT_REF   "6bde108329ff9fc24404b79b8b397261a75cf6a8")
set(ZED_CLAP_SDK_TAG        "1.2.10")  # confirm tag order at m1 when the CLAP target is added
# Tools (not linked)
set(ZED_PLUGINVAL_VERSION   "v1.0.4")
set(ZED_CLAP_VALIDATOR_VERSION "0.4.1")
