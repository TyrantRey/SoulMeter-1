# Copies every .ttf in SRC to DST; warns (does not fail) when there is none,
# matching the MSBuild package step. Fonts are not tracked in git.
file(GLOB _fonts "${SRC}/*.ttf")
if(NOT _fonts)
  message(WARNING "No .ttf in ${SRC}; the package will ship without a font.")
endif()
foreach(_f ${_fonts})
  file(COPY "${_f}" DESTINATION "${DST}")
endforeach()
