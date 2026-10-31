if(NOT DEFINED INPUT_FILE)
    message(FATAL_ERROR "postprocess_web_html.cmake requires INPUT_FILE")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/../third_party/fsui-lib/cmake/postprocess_web_html.cmake")

file(READ "${INPUT_FILE}" html)
file(READ "${CMAKE_CURRENT_LIST_DIR}/file_access.js" file_access)
string(REPLACE "</body>" "<script>\n${file_access}\n</script>\n</body>" html "${html}")
file(WRITE "${INPUT_FILE}" "${html}")
