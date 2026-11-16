# The iPad companion's facade over the shared core (Add-On spec §6.1). Plain
# C++17: it builds and is tested with everything else, and the iPad build
# compiles the same sources for iPadOS (ipad/README.md).
vcc_add_library(vcc_mobile DEPS vcc_session vcc_engraving vcc_editing vcc_project vcc_core)
