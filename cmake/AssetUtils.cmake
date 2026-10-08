# The generators are Python scripts started through their shebang line, which only works where
# commands run through a POSIX shell. Run them through the interpreter explicitly so Ninja on
# Windows (commands go through cmd.exe) works too.
find_package(Python3 COMPONENTS Interpreter QUIET)
if(Python3_Interpreter_FOUND)
  set(PD_ASSET_PYTHON "${Python3_EXECUTABLE}")
else()
  set(PD_ASSET_PYTHON "")
endif()

# execute a header generator (execcmd) for every json file in jsonpath, collect headers in headerlist
# note that this reads ROMID
macro(generate_asset_headers jsonpath execcmd extraarg headerlist)
  set(TMP_JSON "")

  if (${jsonpath} MATCHES "\.json")
    # is a json file
    list(APPEND TMP_JSON "${CMAKE_SOURCE_DIR}/${ASSET_DIR}/${jsonpath}")
  else()
    # is a directory
    file(GLOB TMP_JSON "${CMAKE_SOURCE_DIR}/${ASSET_DIR}/${jsonpath}*.json")
  endif()

  foreach(JSON ${TMP_JSON})
    unset(HEADERNAME)
    string(REPLACE ".json" ".h" HEADERNAME ${JSON})
    string(REPLACE "${CMAKE_SOURCE_DIR}/${ASSET_DIR}" "${CMAKE_BINARY_DIR}/${GENERATED_DIR}" HEADERNAME ${HEADERNAME})
    add_custom_command(
      OUTPUT  ${HEADERNAME}
      DEPENDS ${JSON}
      COMMAND ${PD_ASSET_PYTHON} ${execcmd} ${JSON} ${extraarg} --headers-only --romid=${ROMID}
    )
    list(APPEND ${headerlist} "${HEADERNAME}")
  endforeach()

  unset(TMP_JSON)
endmacro()
