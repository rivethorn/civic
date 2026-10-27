#include "lsp.hpp"

#include "lexer.hpp"
#include "parser.hpp"
#include "sema.hpp"

#include <llvm/Support/JSON.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace civic {
namespace {
using JsonObject = llvm::json::Object;
using JsonValue = llvm::json::Value;

struct Document {
  std::string uri;
  std::filesystem::path path;
  std::string text;
  int64_t version = 0;
};

std::optional<std::string> stringProperty(const JsonObject* object,
                                          llvm::StringRef name) {
  if (!object) return std::nullopt;
  const JsonValue* value = object->get(name);
  if (!value) return std::nullopt;
  auto text = value->getAsString();
  if (!text) return std::nullopt;
  return text->str();
}

const JsonObject* objectProperty(const JsonObject* object, llvm::StringRef name) {
  if (!object) return nullptr;
  const JsonValue* value = object->get(name);
  return value ? value->getAsObject() : nullptr;
}

std::string decodeUriPath(const std::string& uri) {
  std::string path = uri.rfind("file://", 0) == 0 ? uri.substr(7) : uri;
  std::string decoded;
  for (size_t i = 0; i < path.size(); ++i) {
    if (path[i] == '%' && i + 2 < path.size()) {
      auto hexValue = [](char value) -> int {
        if (value >= '0' && value <= '9') return value - '0';
        if (value >= 'a' && value <= 'f') return value - 'a' + 10;
        if (value >= 'A' && value <= 'F') return value - 'A' + 10;
        return -1;
      };
      int high = hexValue(path[i + 1]);
      int low = hexValue(path[i + 2]);
      if (high >= 0 && low >= 0) {
        decoded.push_back(static_cast<char>(high * 16 + low));
        i += 2;
        continue;
      }
    }
    decoded.push_back(path[i]);
  }
#ifdef _WIN32
  if (decoded.size() > 2 && decoded[0] == '/' && decoded[2] == ':') {
    decoded.erase(decoded.begin());
  }
#endif
  return decoded;
}

std::string encodePathUri(const std::filesystem::path& path) {
  std::string raw = std::filesystem::absolute(path).generic_string();
  std::string uri = "file://";
#ifdef _WIN32
  if (!raw.empty() && raw[0] != '/') uri.push_back('/');
#endif
  static const char hex[] = "0123456789ABCDEF";
  for (unsigned char ch : raw) {
    if (std::isalnum(ch) || ch == '/' || ch == ':' || ch == '-' || ch == '_' ||
        ch == '.' || ch == '~') {
      uri.push_back(static_cast<char>(ch));
    } else {
      uri.push_back('%');
      uri.push_back(hex[ch >> 4]);
      uri.push_back(hex[ch & 15]);
    }
  }
  return uri;
}

size_t utf8SequenceLength(std::string_view text, size_t offset) {
  unsigned char first = static_cast<unsigned char>(text[offset]);
  size_t length = first < 0x80 ? 1 : (first & 0xE0) == 0xC0 ? 2 :
                  (first & 0xF0) == 0xE0 ? 3 : (first & 0xF8) == 0xF0 ? 4 : 1;
  if (offset + length > text.size()) return 1;
  for (size_t i = 1; i < length; ++i) {
    if ((static_cast<unsigned char>(text[offset + i]) & 0xC0) != 0x80) return 1;
  }
  return length;
}

uint32_t utf8CodePoint(std::string_view text, size_t offset, size_t length) {
  unsigned char first = static_cast<unsigned char>(text[offset]);
  if (length == 1) return first;
  uint32_t value = first & (length == 2 ? 0x1F : length == 3 ? 0x0F : 0x07);
  for (size_t i = 1; i < length; ++i) {
    value = (value << 6) | (static_cast<unsigned char>(text[offset + i]) & 0x3F);
  }
  return value;
}

size_t utf16Length(std::string_view text) {
  size_t units = 0;
  for (size_t offset = 0; offset < text.size();) {
    size_t length = utf8SequenceLength(text, offset);
    uint32_t codePoint = utf8CodePoint(text, offset, length);
    units += codePoint > 0xFFFF ? 2 : 1;
    offset += length;
  }
  return units;
}

size_t byteColumnForUtf16(std::string_view text, size_t lineNumber,
                          size_t utf16Column) {
  size_t lineStart = 0;
  for (size_t line = 0; line < lineNumber; ++line) {
    size_t newline = text.find('\n', lineStart);
    if (newline == std::string_view::npos) return 0;
    lineStart = newline + 1;
  }
  size_t lineEnd = text.find('\n', lineStart);
  if (lineEnd == std::string_view::npos) lineEnd = text.size();
  size_t units = 0;
  size_t offset = lineStart;
  while (offset < lineEnd && units < utf16Column) {
    size_t length = utf8SequenceLength(text.substr(0, lineEnd), offset);
    uint32_t codePoint = utf8CodePoint(text, offset, length);
    size_t nextUnits = units + (codePoint > 0xFFFF ? 2 : 1);
    if (nextUnits > utf16Column) break;
    units = nextUnits;
    offset += length;
  }
  return offset - lineStart;
}

std::string_view sourceLine(std::string_view text, size_t lineNumber) {
  size_t lineStart = 0;
  for (size_t line = 0; line < lineNumber; ++line) {
    size_t newline = text.find('\n', lineStart);
    if (newline == std::string_view::npos) return {};
    lineStart = newline + 1;
  }
  size_t lineEnd = text.find('\n', lineStart);
  if (lineEnd == std::string_view::npos) lineEnd = text.size();
  return text.substr(lineStart, lineEnd - lineStart);
}

JsonObject position(size_t line, size_t character) {
  JsonObject result;
  result["line"] = static_cast<int64_t>(line);
  result["character"] = static_cast<int64_t>(character);
  return result;
}

JsonObject range(size_t line, size_t start, size_t end) {
  JsonObject result;
  result["start"] = position(line, start);
  result["end"] = position(line, end);
  return result;
}

void sendMessage(JsonValue message) {
  std::string body;
  llvm::raw_string_ostream stream(body);
  message.print(stream);
  stream.flush();
  std::cout << "Content-Length: " << body.size() << "\r\n\r\n" << body << std::flush;
}

void sendResponse(const JsonValue& id, JsonValue result) {
  JsonObject response;
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  response["result"] = std::move(result);
  sendMessage(std::move(response));
}

void sendError(const JsonValue& id, int code, const std::string& message) {
  JsonObject detail;
  detail["code"] = code;
  detail["message"] = message;
  JsonObject response;
  response["jsonrpc"] = "2.0";
  response["id"] = id;
  response["error"] = std::move(detail);
  sendMessage(std::move(response));
}

std::optional<std::string> readMessage() {
  std::string line;
  size_t contentLength = 0;
  bool foundLength = false;
  while (std::getline(std::cin, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty()) break;
    const std::string header = "Content-Length:";
    if (line.size() >= header.size()) {
      std::string key = line.substr(0, header.size());
      std::transform(key.begin(), key.end(), key.begin(),
                     [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
      if (key == "content-length:") {
        try {
          contentLength = static_cast<size_t>(std::stoull(line.substr(header.size())));
          foundLength = true;
        } catch (const std::exception&) {
          throw std::runtime_error("invalid Content-Length header");
        }
      }
    }
  }
  if (!std::cin && line.empty()) return std::nullopt;
  if (!foundLength) throw std::runtime_error("JSON-RPC message is missing Content-Length");
  if (contentLength > 16 * 1024 * 1024) {
    throw std::runtime_error("JSON-RPC message exceeds the 16 MiB limit");
  }
  std::string body(contentLength, '\0');
  std::cin.read(body.data(), static_cast<std::streamsize>(contentLength));
  if (static_cast<size_t>(std::cin.gcount()) != contentLength) {
    throw std::runtime_error("truncated JSON-RPC message body");
  }
  return body;
}

std::optional<Token> tokenAt(const Document& document, size_t line, size_t character) {
  try {
    std::vector<Token> tokens = Lexer(document.text, document.path.string()).tokenize();
    size_t byteColumn = byteColumnForUtf16(document.text, line, character);
    for (const Token& token : tokens) {
      if (token.kind == Token::Kind::End || token.location.line != line + 1) continue;
      size_t start = token.location.column - 1;
      if (start <= byteColumn && byteColumn < start + token.text.size()) return token;
    }
  } catch (const Diagnostic&) {
  }
  return std::nullopt;
}

JsonValue diagnosticsFor(const Document& document, const std::string& standardLibraryPath) {
  llvm::json::Array diagnostics;
  try {
    Program program = parseSourceText(document.text, document.path.string(),
                                      standardLibraryPath);
    validateProgram(program);
  } catch (const Diagnostic& diagnostic) {
    std::filesystem::path diagnosticPath = diagnostic.location.file.empty()
                                               ? document.path
                                               : std::filesystem::path(diagnostic.location.file);
    std::error_code error;
    std::error_code sourceError;
    std::filesystem::path sourceCanonical =
      std::filesystem::weakly_canonical(diagnosticPath, error);
    std::filesystem::path documentCanonical =
      std::filesystem::weakly_canonical(document.path, sourceError);
    if (!error && !sourceError && sourceCanonical == documentCanonical) {
    JsonObject item;
    size_t line = diagnostic.location.line > 0 ? diagnostic.location.line - 1 : 0;
    std::string_view textLine = sourceLine(document.text, line);
    size_t byteCharacter = diagnostic.location.column > 0
                               ? diagnostic.location.column - 1
                               : 0;
    size_t character = utf16Length(textLine.substr(
        0, std::min(byteCharacter, textLine.size())));
      item["range"] = range(line, character, character + 1);
      item["severity"] = 1;
      item["source"] = "civic";
      item["message"] = diagnostic.what();
      diagnostics.push_back(std::move(item));
    }
  } catch (const std::exception& error) {
    JsonObject item;
    item["range"] = range(0, 0, 1);
    item["severity"] = 1;
    item["source"] = "civic";
    item["message"] = error.what();
    diagnostics.push_back(std::move(item));
  }
  return diagnostics;
}

void publishDiagnostics(const Document& document,
                        const std::string& standardLibraryPath) {
  JsonObject params;
  params["uri"] = document.uri;
  params["diagnostics"] = diagnosticsFor(document, standardLibraryPath);
  JsonObject notification;
  notification["jsonrpc"] = "2.0";
  notification["method"] = "textDocument/publishDiagnostics";
  notification["params"] = std::move(params);
  sendMessage(std::move(notification));
}

JsonValue completionItems(const Document& document,
                          const std::string& standardLibraryPath) {
  std::map<std::string, std::pair<int, std::string>> items;
  for (const std::string& keyword : {"import", "return", "if", "else", "while", "for"}) {
    items[keyword] = {14, "Civic keyword"};
  }
  for (const std::string& type : {"int", "float", "double", "bool", "String", "void"}) {
    items[type] = {25, "Civic type"};
  }
  items["assert"] = {3, "assert(bool | int) -> void"};
  items["print"] = {3, "print(...values) -> void; requires import std.io"};
  items["println"] = {3, "println(...values) -> void; requires import std.io"};
  try {
    Program program = parseSourceText(document.text, document.path.string(),
                                      standardLibraryPath);
    for (const FunctionDecl& function : program.functions) {
      std::string signature = function.returnType + " " + function.name + "(";
      for (size_t i = 0; i < function.parameters.size(); ++i) {
        if (i) signature += ", ";
        signature += function.parameters[i].typeName + " " + function.parameters[i].name;
      }
      signature += ")";
      items[function.name] = {3, std::move(signature)};
    }
  } catch (const Diagnostic&) {
  }
  llvm::json::Array result;
  for (const auto& entry : items) {
    JsonObject item;
    item["label"] = entry.first;
    item["kind"] = entry.second.first;
    item["detail"] = entry.second.second;
    result.push_back(std::move(item));
  }
  return result;
}

JsonValue hoverResult(const Document& document, size_t line, size_t character,
                      const std::string& standardLibraryPath) {
  std::optional<Token> token = tokenAt(document, line, character);
  if (!token || token->kind != Token::Kind::Identifier) return nullptr;
  std::string contents;
  try {
    Program program = parseSourceText(document.text, document.path.string(),
                                      standardLibraryPath);
    for (const FunctionDecl& function : program.functions) {
      if (function.name != token->text) continue;
      contents = function.returnType + " " + function.name + "(";
      for (size_t i = 0; i < function.parameters.size(); ++i) {
        if (i) contents += ", ";
        contents += function.parameters[i].typeName + " " +
                    function.parameters[i].name;
      }
      contents += ")";
      break;
    }
  } catch (const Diagnostic&) {
  }
  if (contents.empty()) {
    static const std::map<std::string, std::string> descriptions{
        {"int", "32-bit signed integer"}, {"float", "64-bit floating-point number"},
        {"double", "64-bit floating-point number"}, {"bool", "Boolean value"},
        {"String", "String-literal-backed Civic string"},
        {"void", "No return value"},
        {"assert", "Terminates the program when its condition is false"},
        {"print", "Prints values to standard output; requires import std.io"},
        {"println", "Prints values followed by a newline; requires import std.io"}};
    auto description = descriptions.find(token->text);
    if (description != descriptions.end()) contents = description->second;
  }
  if (contents.empty()) return nullptr;
  JsonObject markup;
  markup["kind"] = "markdown";
  markup["value"] = "```civic\n" + contents + "\n```";
  JsonObject result;
  result["contents"] = std::move(markup);
  return result;
}

JsonValue definitionResult(const Document& document, size_t line, size_t character,
                          const std::string& standardLibraryPath) {
  std::optional<Token> token = tokenAt(document, line, character);
  if (!token || token->kind != Token::Kind::Identifier) return nullptr;
  try {
    Program program = parseSourceText(document.text, document.path.string(),
                                      standardLibraryPath);
    for (const FunctionDecl& function : program.functions) {
      if (function.name != token->text) continue;
      std::filesystem::path definitionPath = function.location.file.empty()
                                                 ? document.path
                                                 : std::filesystem::path(function.location.file);
      std::error_code documentError;
      std::error_code definitionError;
      bool isCurrentDocument =
          std::filesystem::weakly_canonical(definitionPath, definitionError) ==
          std::filesystem::weakly_canonical(document.path, documentError);
      std::string definitionText;
      if (isCurrentDocument) {
        definitionText = document.text;
      } else {
        std::ifstream input(definitionPath);
        if (!input) continue;
        definitionText.assign(std::istreambuf_iterator<char>(input),
                              std::istreambuf_iterator<char>());
      }
      std::vector<Token> tokens =
          Lexer(definitionText, definitionPath.string()).tokenize();
      for (size_t i = 0; i + 1 < tokens.size(); ++i) {
        if (tokens[i].kind != Token::Kind::Identifier ||
            tokens[i].text != function.name || tokens[i + 1].text != "(") {
          continue;
        }
        JsonObject location;
        location["uri"] = encodePathUri(definitionPath);
        size_t definitionLine = tokens[i].location.line - 1;
        size_t definitionCharacter = utf16Length(sourceLine(
            definitionText, definitionLine).substr(0, tokens[i].location.column - 1));
        location["range"] = range(tokens[i].location.line - 1,
                                  definitionCharacter,
                                  definitionCharacter + utf16Length(function.name));
        llvm::json::Array result;
        result.push_back(std::move(location));
        return result;
      }
    }
  } catch (const Diagnostic&) {
  }
  return nullptr;
}

std::optional<int64_t> integerProperty(const JsonObject* object,
                                       llvm::StringRef name) {
  if (!object) return std::nullopt;
  const JsonValue* value = object->get(name);
  return value ? value->getAsInteger() : std::nullopt;
}

void handleMessage(JsonValue message, std::map<std::string, Document>& documents,
                   const std::string& standardLibraryPath, bool& shutdown,
                   bool& exited) {
  const JsonObject* request = message.getAsObject();
  if (!request) return;
  const JsonValue* id = request->get("id");
  auto method = stringProperty(request, "method");
  if (!method) return;
  const JsonObject* params = objectProperty(request, "params");
  if (*method == "initialize") {
    JsonObject capabilities;
    capabilities["textDocumentSync"] = 1;
    capabilities["completionProvider"] = JsonObject{{"resolveProvider", false}};
    capabilities["hoverProvider"] = true;
    capabilities["definitionProvider"] = true;
    JsonObject serverInfo;
    serverInfo["name"] = "civic";
    serverInfo["version"] = "0.1.0";
    JsonObject result;
    result["capabilities"] = std::move(capabilities);
    result["serverInfo"] = std::move(serverInfo);
    if (id) sendResponse(*id, std::move(result));
    return;
  }
  if (*method == "initialized" || *method == "$/cancelRequest") return;
  if (*method == "shutdown") {
    shutdown = true;
    if (id) sendResponse(*id, nullptr);
    return;
  }
  if (*method == "exit") {
    exited = true;
    return;
  }
  if (*method == "textDocument/didOpen" ||
      *method == "textDocument/didChange") {
    const JsonObject* textDocument = objectProperty(params, "textDocument");
    auto uri = stringProperty(textDocument, "uri");
    if (!uri) return;
    Document& document = documents[*uri];
    auto incomingVersion = integerProperty(textDocument, "version");
    if (*method == "textDocument/didChange" && incomingVersion &&
        *incomingVersion < document.version) {
      return;
    }
    document.uri = *uri;
    document.path = std::filesystem::weakly_canonical(decodeUriPath(*uri));
    if (incomingVersion) {
      document.version = *incomingVersion;
    }
    if (*method == "textDocument/didOpen") {
      if (auto text = stringProperty(textDocument, "text")) document.text = *text;
    } else if (params) {
      const JsonValue* changesValue = params->get("contentChanges");
      const llvm::json::Array* changes =
          changesValue ? changesValue->getAsArray() : nullptr;
      if (changes) {
        for (const JsonValue& change : *changes) {
          auto text = stringProperty(change.getAsObject(), "text");
          if (text) document.text = *text;
        }
      }
    }
    publishDiagnostics(document, standardLibraryPath);
    return;
  }
  if (*method == "textDocument/didClose") {
    const JsonObject* textDocument = objectProperty(params, "textDocument");
    auto uri = stringProperty(textDocument, "uri");
    if (!uri) return;
    documents.erase(*uri);
    JsonObject clearParams;
    clearParams["uri"] = *uri;
    clearParams["diagnostics"] = llvm::json::Array();
    JsonObject notification;
    notification["jsonrpc"] = "2.0";
    notification["method"] = "textDocument/publishDiagnostics";
    notification["params"] = std::move(clearParams);
    sendMessage(std::move(notification));
    return;
  }
  if (*method == "textDocument/completion" || *method == "textDocument/hover" ||
      *method == "textDocument/definition") {
    const JsonObject* textDocument = objectProperty(params, "textDocument");
    auto uri = stringProperty(textDocument, "uri");
    if (!id) return;
    if (!uri || documents.find(*uri) == documents.end()) {
      sendResponse(*id, nullptr);
      return;
    }
    const JsonObject* positionObject = objectProperty(params, "position");
    size_t line = static_cast<size_t>(std::max<int64_t>(
        0, integerProperty(positionObject, "line").value_or(0)));
    size_t character = static_cast<size_t>(std::max<int64_t>(
        0, integerProperty(positionObject, "character").value_or(0)));
    const Document& document = documents.at(*uri);
    if (*method == "textDocument/completion") {
      sendResponse(*id, completionItems(document, standardLibraryPath));
    } else if (*method == "textDocument/hover") {
      sendResponse(*id, hoverResult(document, line, character, standardLibraryPath));
    } else {
      sendResponse(*id, definitionResult(document, line, character,
                                          standardLibraryPath));
    }
    return;
  }
  if (id) sendError(*id, -32601, "Method not found");
}
}  // namespace

int runLanguageServer(const std::string& standardLibraryPath) {
  std::map<std::string, Document> documents;
  bool shutdown = false;
  bool exited = false;
  try {
    while (!exited) {
      std::optional<std::string> body = readMessage();
      if (!body) break;
      auto parsed = llvm::json::parse(*body);
      if (!parsed) {
        llvm::consumeError(parsed.takeError());
        sendError(nullptr, -32700, "Parse error");
        continue;
      }
      handleMessage(std::move(*parsed), documents, standardLibraryPath, shutdown,
                    exited);
    }
  } catch (const std::exception& error) {
    std::cerr << "civic lsp: " << error.what() << '\n';
    return 1;
  }
  return shutdown ? 0 : 1;
}
}  // namespace civic
