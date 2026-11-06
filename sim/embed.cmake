# Turns web/index.html into a C++ header, so the simulator is one file to run.
file(READ "${IN}" CONTENT)
file(WRITE "${OUT}" "// Made from sim/web/index.html at build time. Don't edit.\n#pragma once\nnamespace leota_sim {\ninline constexpr const char* kWebPage = R\"LEOTAPAGE(")
file(APPEND "${OUT}" "${CONTENT}")
file(APPEND "${OUT}" ")LEOTAPAGE\";\n}  // namespace leota_sim\n")
