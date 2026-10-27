file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}")
set(DOCUMENT_PATH "${TEST_ROOT}/sample.cv")
set(DOCUMENT_URI "file://${DOCUMENT_PATH}")
set(INPUT_PATH "${TEST_ROOT}/messages.jsonrpc")
file(WRITE "${INPUT_PATH}" "")

function(append_message body)
  string(LENGTH "${body}" body_length)
  file(APPEND "${INPUT_PATH}"
    "Content-Length: ${body_length}\r\n\r\n${body}")
endfunction()

append_message("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}")
append_message("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didOpen\",\"params\":{\"textDocument\":{\"uri\":\"${DOCUMENT_URI}\",\"languageId\":\"civic\",\"version\":1,\"text\":\"int square(int value) { return value * value; }\\nint main() { return square(3); }\\n\"}}}")
append_message("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"textDocument/completion\",\"params\":{\"textDocument\":{\"uri\":\"${DOCUMENT_URI}\"},\"position\":{\"line\":1,\"character\":22}}}")
append_message("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"textDocument/hover\",\"params\":{\"textDocument\":{\"uri\":\"${DOCUMENT_URI}\"},\"position\":{\"line\":0,\"character\":5}}}")
append_message("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"textDocument/definition\",\"params\":{\"textDocument\":{\"uri\":\"${DOCUMENT_URI}\"},\"position\":{\"line\":1,\"character\":22}}}")
append_message("{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/didChange\",\"params\":{\"textDocument\":{\"uri\":\"${DOCUMENT_URI}\",\"version\":2},\"contentChanges\":[{\"text\":\"int main() { return mystery; }\\n\"}]}}")
append_message("{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"shutdown\",\"params\":null}")
append_message("{\"jsonrpc\":\"2.0\",\"method\":\"exit\"}")

execute_process(
  COMMAND "${CIVIC_EXECUTABLE}" lsp
  INPUT_FILE "${INPUT_PATH}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "civic lsp exited unsuccessfully: ${error}${output}")
endif()
foreach(expected IN ITEMS
    "\"serverInfo\":{\"name\":\"civic\""
    "\"label\":\"square\""
    "int square(int value)"
    "\"uri\":\"${DOCUMENT_URI}\""
    "unknown variable")
  string(FIND "${output}" "${expected}" found)
  if(found EQUAL -1)
    message(FATAL_ERROR "Language Server response did not contain '${expected}': ${error}${output}")
  endif()
endforeach()
string(FIND "${output}" "\"id\":4" definition_response)
if(definition_response EQUAL -1)
  message(FATAL_ERROR "Language Server did not answer the definition request: ${output}")
endif()
string(SUBSTRING "${output}" ${definition_response} -1 definition_output)
string(FIND "${definition_output}" "\"uri\":\"${DOCUMENT_URI}\"" definition_uri)
if(definition_uri EQUAL -1)
  message(FATAL_ERROR "Function definition response did not contain the source location: ${output}")
endif()

file(REMOVE_RECURSE "${TEST_ROOT}")
