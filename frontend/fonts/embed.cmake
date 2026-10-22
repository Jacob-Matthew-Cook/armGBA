# cmake -DFONTS=a.otf|b.ttf -DOUT=fonts.inc -P embed.cmake: the frontend's fonts as C arrays, each
# named font and its file's name without dashes.
string(REPLACE "|" ";" FONTS "${FONTS}")
file(WRITE "${OUT}" "")
foreach(font ${FONTS})
	get_filename_component(name "${font}" NAME_WE)
	string(REPLACE "-" "" name "${name}")
	file(READ "${font}" hex HEX)
	string(REGEX REPLACE "(..)" "0x\\1," hex "${hex}")
	file(APPEND "${OUT}" "static const unsigned char font${name}[] = {${hex}};\n")
endforeach()
