#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "ast.hpp"
#include "codegen.hpp"
#include "fmt.hpp"
#include "lsp.hpp"
#include "parser.hpp"
#include "sema.hpp"
#include "version.hpp"

#include "codegen.cpp"
#include "fmt.cpp"
#include "lexer.cpp"
#include "lsp.cpp"
#include "parser.cpp"
#include "sema.cpp"

namespace fs = std::filesystem;

namespace {
class TemporaryDirectory {
public:
	TemporaryDirectory() {
		static std::atomic<unsigned long long> serial{0};
		for (int attempt = 0; attempt < 100; ++attempt) {
			auto stamp =
				std::chrono::steady_clock::now().time_since_epoch().count();
			path_ = fs::temp_directory_path() /
					("civic-" + std::to_string(stamp) + "-" +
					 std::to_string(serial.fetch_add(1)));
			std::error_code error;
			if (fs::create_directory(path_, error))
				return;
			if (error)
				throw std::runtime_error("cannot create temporary directory: " +
										 error.message());
		}
		throw std::runtime_error("cannot create a unique temporary directory");
	}
	~TemporaryDirectory() {
		std::error_code error;
		fs::remove_all(path_, error);
	}
	const fs::path& path() const {
		return path_;
	}

private:
	fs::path path_;
};

int runProcess(const std::vector<std::string>& arguments) {
	if (arguments.empty())
		return 127;
#ifdef _WIN32
	auto quote = [](const std::string& argument) {
		std::string result = "\"";
		size_t slashes	   = 0;
		for (char ch : argument) {
			if (ch == '\\') {
				++slashes;
			} else if (ch == '"') {
				result.append(slashes * 2 + 1, '\\');
				result.push_back('"');
				slashes = 0;
			} else {
				result.append(slashes, '\\');
				slashes = 0;
				result.push_back(ch);
			}
		}
		result.append(slashes * 2, '\\');
		result.push_back('"');
		return result;
	};
	std::string commandLine;
	for (const std::string& argument : arguments) {
		if (!commandLine.empty())
			commandLine.push_back(' ');
		commandLine += quote(argument);
	}
	std::vector<char> mutableCommand(commandLine.begin(), commandLine.end());
	mutableCommand.push_back('\0');
	STARTUPINFOA startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	if (!CreateProcessA(nullptr, mutableCommand.data(), nullptr, nullptr, FALSE,
						0, nullptr, nullptr, &startup, &process)) {
		throw std::runtime_error("cannot start process (Windows error " +
								 std::to_string(GetLastError()) + ")");
	}
	if (WaitForSingleObject(process.hProcess, INFINITE) != WAIT_OBJECT_0) {
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		throw std::runtime_error("cannot wait for child process");
	}
	DWORD status = 1;
	if (!GetExitCodeProcess(process.hProcess, &status)) {
		CloseHandle(process.hThread);
		CloseHandle(process.hProcess);
		throw std::runtime_error("cannot read child process exit status");
	}
	CloseHandle(process.hThread);
	CloseHandle(process.hProcess);
	return static_cast<int>(status);
#else
	pid_t child = fork();
	if (child < 0)
		throw std::runtime_error("cannot start process");
	if (child == 0) {
		std::vector<char*> argv;
		argv.reserve(arguments.size() + 1);
		for (const std::string& argument : arguments) {
			argv.push_back(const_cast<char*>(argument.c_str()));
		}
		argv.push_back(nullptr);
		execvp(argv[0], argv.data());
		std::cerr << "civic: cannot execute " << arguments[0] << ": "
				  << std::strerror(errno) << '\n';
		_exit(127);
	}
	int status = 0;
	while (waitpid(child, &status, 0) < 0) {
		if (errno != EINTR)
			throw std::runtime_error("cannot wait for child process");
	}
	if (WIFEXITED(status))
		return WEXITSTATUS(status);
	if (WIFSIGNALED(status))
		return 128 + WTERMSIG(status);
	return 1;
#endif
}

std::string readText(const fs::path& path) {
	std::ifstream input(path);
	if (!input)
		throw std::runtime_error("cannot read source file: " + path.string());
	std::ostringstream content;
	content << input.rdbuf();
	return content.str();
}

void writeText(const fs::path& path, const std::string& contents) {
	std::ofstream output(path);
	if (!output)
		throw std::runtime_error("cannot write file: " + path.string());
	output << contents;
	if (!output)
		throw std::runtime_error("failed writing file: " + path.string());
}

std::string compiler() {
#ifdef CIVIC_CLANG_PATH
	return CIVIC_CLANG_PATH;
#else
	const char* configured = std::getenv("CXX");
	return configured && *configured ? configured : "clang++";
#endif
}

std::string executableSuffix() {
#ifdef CIVIC_EXECUTABLE_SUFFIX
	return CIVIC_EXECUTABLE_SUFFIX;
#else
	return "";
#endif
}

fs::path executablePath(const char* argv0) {
#ifdef _WIN32
	std::vector<char> buffer(32768, '\0');
	DWORD length = GetModuleFileNameA(nullptr, buffer.data(),
									  static_cast<DWORD>(buffer.size()));
	if (length == 0 || length == buffer.size()) {
		throw std::runtime_error("cannot determine Civic executable path");
	}
	return fs::weakly_canonical(fs::path(std::string(buffer.data(), length)));
#else
	std::error_code error;
	fs::path resolved = fs::canonical("/proc/self/exe", error);
	if (!error)
		return resolved;
	fs::path invoked(argv0);
	if (invoked.has_parent_path()) {
		return fs::weakly_canonical(fs::absolute(invoked));
	}
	const char* pathValue = std::getenv("PATH");
	if (pathValue) {
		std::string searchPath(pathValue);
		size_t start = 0;
		while (start <= searchPath.size()) {
			size_t end			  = searchPath.find(':', start);
			std::string directory = searchPath.substr(
				start,
				end == std::string::npos ? std::string::npos : end - start);
			fs::path candidate =
				(directory.empty() ? fs::current_path() : fs::path(directory)) /
				invoked;
			if (fs::is_regular_file(candidate))
				return fs::weakly_canonical(candidate);
			if (end == std::string::npos)
				break;
			start = end + 1;
		}
	}
	throw std::runtime_error("cannot determine Civic executable path");
#endif
}

fs::path resolveCivicRoot(const char* argv0) {
	const char* configured = std::getenv("CIVIC_ROOT");
	if (configured && *configured) {
		fs::path root = fs::weakly_canonical(configured);
		if (!fs::is_directory(root / "stdlib")) {
			throw std::runtime_error(
				"CIVIC_ROOT does not contain a stdlib directory: " +
				root.string());
		}
		return root;
	}

	fs::path executable = executablePath(argv0);
	std::vector<fs::path> candidates{
		executable.parent_path().parent_path(),
		executable.parent_path().parent_path() / "share" / "civic",
		executable.parent_path() / "share" / "civic", executable.parent_path()};
	for (const fs::path& candidate : candidates) {
		if (fs::is_directory(candidate / "stdlib") &&
			(fs::exists(candidate / "CMakeLists.txt") ||
			 fs::exists(candidate / ".civic-root"))) {
			return fs::weakly_canonical(candidate);
		}
	}
	throw std::runtime_error("cannot find Civic root; set CIVIC_ROOT to a "
							 "directory containing stdlib/");
}

int buildProgram(const fs::path& input, const fs::path& output,
				 const std::string& mode, const fs::path& standardLibraryPath,
				 const std::vector<std::string>& programArguments = {},
				 bool release									  = false) {
	civic::Program program =
		civic::parseSourceFile(input.string(), standardLibraryPath.string());
	civic::validateProgram(program);
	const std::string ir =
		civic::generateLLVMIR(program, input.filename().string());
	TemporaryDirectory temporary;
	fs::path irPath = temporary.path() / "program.ll";
	writeText(irPath, ir);
	std::vector<std::string> command{compiler(), "-x", "ir", irPath.string(),
									 release ? "-O2" : "-g"};
	if (mode == "object")
		command.push_back("-c");
	command.push_back("-o");
	command.push_back(output.string());
	int result = runProcess(command);
	if (result != 0)
		return result;
	if (mode == "run") {
		std::vector<std::string> runArguments{fs::absolute(output).string()};
		runArguments.insert(runArguments.end(), programArguments.begin(),
							programArguments.end());
		return runProcess(runArguments);
	}
	return 0;
}

struct Project {
	fs::path root;
	std::string name;
	std::string version;
};

std::string trim(std::string text) {
	const std::string whitespace = " \t\r\n";
	size_t first				 = text.find_first_not_of(whitespace);
	if (first == std::string::npos)
		return {};
	size_t last = text.find_last_not_of(whitespace);
	return text.substr(first, last - first + 1);
}

std::string parseTomlString(const std::string& value, const fs::path& manifest,
							size_t lineNumber) {
	if (value.size() < 2 || value.front() != '"' || value.back() != '"') {
		throw std::runtime_error(manifest.string() + ":" +
								 std::to_string(lineNumber) +
								 ": expected a quoted string");
	}
	return value.substr(1, value.size() - 2);
}

Project loadProject(const fs::path& manifestPath) {
	std::ifstream input(manifestPath);
	if (!input)
		throw std::runtime_error("cannot read project manifest: " +
								 manifestPath.string());
	Project project;
	project.root	= fs::absolute(manifestPath).parent_path();
	project.version = "0.1.0";
	bool inPackage	= false;
	std::string line;
	size_t lineNumber = 0;
	while (std::getline(input, line)) {
		++lineNumber;
		std::string text = trim(line);
		if (text.empty() || text[0] == '#')
			continue;
		if (text.front() == '[' && text.back() == ']') {
			inPackage = text == "[package]";
			continue;
		}
		if (!inPackage)
			continue;
		size_t equals = text.find('=');
		if (equals == std::string::npos) {
			throw std::runtime_error(manifestPath.string() + ":" +
									 std::to_string(lineNumber) +
									 ": expected key = value");
		}
		std::string key	  = trim(text.substr(0, equals));
		std::string value = trim(text.substr(equals + 1));
		size_t comment	  = value.find(" #");
		if (comment != std::string::npos)
			value = trim(value.substr(0, comment));
		if (key == "name") {
			project.name = parseTomlString(value, manifestPath, lineNumber);
		} else if (key == "version") {
			project.version = parseTomlString(value, manifestPath, lineNumber);
		}
	}
	if (project.name.empty()) {
		throw std::runtime_error(manifestPath.string() +
								 ": [package] is missing a name");
	}
	return project;
}

fs::path locateManifest(const std::optional<fs::path>& requested) {
	if (requested) {
		fs::path manifest = *requested;
		if (fs::is_directory(manifest))
			manifest /= "civic.toml";
		if (!fs::is_regular_file(manifest)) {
			throw std::runtime_error("project manifest not found: " +
									 manifest.string());
		}
		return fs::absolute(manifest);
	}

	fs::path directory = fs::current_path();
	while (true) {
		fs::path candidate = directory / "civic.toml";
		if (fs::is_regular_file(candidate))
			return candidate;
		fs::path parent = directory.parent_path();
		if (parent == directory)
			break;
		directory = std::move(parent);
	}
	throw std::runtime_error(
		"no civic.toml found; run 'civic new <name>' to create a project");
}

bool validPackageName(const std::string& name) {
	if (name.empty())
		return false;
	for (char ch : name) {
		if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '-' &&
			ch != '_')
			return false;
	}
	return true;
}

int createProject(const fs::path& destination) {
	fs::path root	 = fs::absolute(destination);
	std::string name = root.filename().string();
	if (!validPackageName(name)) {
		throw std::runtime_error(
			"project name must contain only letters, digits, '-' or '_'");
	}
	if (fs::exists(root)) {
		throw std::runtime_error("destination already exists: " +
								 root.string());
	}
	fs::create_directories(root / "src");
	writeText(root / "civic.toml",
			  "[package]\nname = \"" + name + "\"\nversion = \"0.1.0\"\n");
	writeText(root / ".gitignore", "/target/\n");
	writeText(root / "src" / "main.cv",
			  "#include <std/io>\n\n"
			  "int main(int argc, string* argv) {\n"
			  "  if (argc > 1) {\n"
			  "    println(\"Hello, \", argv[1], \"!\");\n"
			  "  } else {\n"
			  "    println(\"Hello from " +
				  name +
				  "!\");\n"
				  "  }\n"
				  "  return 0;\n"
				  "}\n");
	std::cout << "created project " << name << " at " << root.string() << '\n';
	return 0;
}

int runProjectCommand(const std::string& command,
					  const std::vector<std::string>& arguments,
					  fs::path& sourcePath,
					  const fs::path& standardLibraryPath) {
	bool release = false;
	std::optional<fs::path> requestedManifest;
	std::vector<std::string> programArguments;
	bool forwarding = false;
	for (size_t i = 0; i < arguments.size(); ++i) {
		const std::string& argument = arguments[i];
		if (forwarding) {
			programArguments.push_back(argument);
		} else if (argument == "--") {
			if (command != "run")
				throw std::runtime_error(
					"'--' program arguments are only valid with civic run");
			forwarding = true;
		} else if (argument == "--release") {
			release = true;
		} else if (argument == "--manifest-path" && i + 1 < arguments.size()) {
			requestedManifest = arguments[++i];
		} else {
			throw std::runtime_error("unexpected argument '" + argument + "'");
		}
	}

	Project project = loadProject(locateManifest(requestedManifest));
	sourcePath		= project.root / "src" / "main.cv";
	if (!fs::is_regular_file(sourcePath)) {
		throw std::runtime_error("project entry point not found: " +
								 sourcePath.string());
	}
	fs::path profile =
		project.root / "target" / (release ? "release" : "debug");
	fs::create_directories(profile);
	fs::path executable = profile / (project.name + executableSuffix());
	int result =
		buildProgram(sourcePath, executable, command == "run" ? "run" : "build",
					 standardLibraryPath, programArguments, release);
	if (command == "build" && result == 0) {
		std::cout << "built " << executable.string() << '\n';
	}
	return result;
}

void printDiagnosticText(const civic::Diagnostic& diagnostic,
						 const std::string& text) {
	std::cerr << '<'
			  << (diagnostic.location.file.empty() ? std::string("stdin")
												   : diagnostic.location.file)
			  << ':' << diagnostic.location.line << ':'
			  << diagnostic.location.column << ": error: " << diagnostic.what()
			  << '\n';
	std::istringstream lines(text);
	std::string line;
	size_t number = 1;
	while (std::getline(lines, line)) {
		if (number == diagnostic.location.line) {
			std::cerr << "  " << line << "\n  "
					  << std::string(diagnostic.location.column > 1
										 ? diagnostic.location.column - 1
										 : 0,
									 ' ')
					  << "^\n";
			break;
		}
		++number;
	}
}

void printDiagnostic(const civic::Diagnostic& diagnostic,
					 const fs::path& input) {
	fs::path diagnosticPath = diagnostic.location.file.empty()
								  ? input
								  : fs::path(diagnostic.location.file);
	std::cerr << diagnosticPath.string() << ':' << diagnostic.location.line
			  << ':' << diagnostic.location.column
			  << ": error: " << diagnostic.what() << '\n';
	std::istringstream lines(readText(diagnosticPath));
	std::string line;
	size_t number = 1;
	while (std::getline(lines, line)) {
		if (number == diagnostic.location.line) {
			std::cerr << "  " << line << "\n  "
					  << std::string(diagnostic.location.column > 1
										 ? diagnostic.location.column - 1
										 : 0,
									 ' ')
					  << "^\n";
			break;
		}
		++number;
	}
}

std::vector<fs::path> discoverTests(const std::vector<std::string>& requested) {
	std::vector<fs::path> files;
	std::vector<fs::path> roots;
	if (requested.empty()) {
		roots.emplace_back("tests");
	} else {
		for (const std::string& item : requested)
			roots.emplace_back(item);
	}
	for (const fs::path& root : roots) {
		if (!fs::exists(root) && requested.empty())
			continue;
		if (fs::is_regular_file(root)) {
			if (root.extension() == ".cv")
				files.push_back(root);
			continue;
		}
		if (!fs::is_directory(root)) {
			throw std::runtime_error("test path does not exist: " +
									 root.string());
		}
		for (const auto& entry : fs::recursive_directory_iterator(root)) {
			if (entry.is_regular_file() && entry.path().extension() == ".cv") {
				std::ifstream input(entry.path());
				std::string firstLine;
				std::getline(input, firstLine);
				if (firstLine.rfind("#header", 0) == 0)
					continue;
				files.push_back(entry.path());
			}
		}
	}
	std::sort(files.begin(), files.end());
	files.erase(std::unique(files.begin(), files.end()), files.end());
	return files;
}

int runTests(const std::vector<std::string>& paths,
			 const fs::path& standardLibraryPath) {
	std::vector<fs::path> tests = discoverTests(paths);
	if (tests.empty()) {
		if (paths.empty()) {
			std::cout << "0 tests found\n";
			return 0;
		}
		std::cerr << "civic test: no .cv tests found\n";
		return 1;
	}
	size_t passed = 0;
	for (const fs::path& test : tests) {
		try {
			TemporaryDirectory temporary;
			fs::path executable =
				temporary.path() / ("test" + executableSuffix());
			int result =
				buildProgram(test, executable, "run", standardLibraryPath);
			if (result != 0) {
				std::cerr << "FAIL " << test.string() << " (exit " << result
						  << ")\n";
				continue;
			}
			++passed;
			std::cout << "PASS " << test.string() << '\n';
		} catch (const civic::Diagnostic& diagnostic) {
			printDiagnostic(diagnostic, test);
			std::cerr << "FAIL " << test.string() << '\n';
		} catch (const std::exception& error) {
			std::cerr << "FAIL " << test.string() << ": " << error.what()
					  << '\n';
		}
	}
	std::cout << passed << '/' << tests.size() << " tests passed\n";
	return passed == tests.size() ? 0 : 1;
}

void usage(std::ostream& out) {
	out << "Usage:\n"
		<< "  civic new <name>\n"
		<< "  civic build [--release] [--manifest-path <path>]\n"
		<< "  civic run [--release] [--manifest-path <path>] [-- <args...>]\n"
		<< "  civic build <source.cv> [-o <output>]\n"
		<< "  civic run <source.cv> [-- <args...>]\n"
		<< "  civic test [file.cv|directory ...]\n"
		<< "  civic check <source.cv>\n"
		<< "  civic fmt [--check] <file.cv>...\n"
		<< "  civic fmt --stdin [(-|<file.cv>)]\n"
		<< "  civic emit-ir <source.cv> [-o <output.ll>]\n"
		<< "  civic lsp  (Language Server Protocol over stdio)\n"
		<< "  civic --version\n"
		<< "  CIVIC_ROOT may point to a Civic root containing stdlib/.\n";
}

} // namespace

int main(int argc, char** argv) {
	fs::path civicRoot;
	try {
		civicRoot = resolveCivicRoot(argv[0]);
	} catch (const std::exception& error) {
		std::cerr << "civic: " << error.what() << '\n';
		return 1;
	}
	const fs::path standardLibraryPath = civicRoot / "stdlib";
	if (argc < 2 || std::string(argv[1]) == "--help" ||
		std::string(argv[1]) == "-h") {
		usage(argc < 2 ? std::cerr : std::cout);
		return argc < 2 ? 2 : 0;
	}
	const std::string command = argv[1];
	if (command == "--version" || command == "-v" || command == "version") {
		std::cout << "civic " << civic::kCivicVersion << '\n';
		return 0;
	}
	if (command == "lsp") {
		if (argc != 2) {
			std::cerr << "civic lsp: does not accept command-line arguments\n";
			return 2;
		}
		return civic::runLanguageServer(standardLibraryPath.string());
	}
	if (command == "new") {
		if (argc != 3) {
			std::cerr << "civic new: expected a project name\n";
			return 2;
		}
		try {
			return createProject(argv[2]);
		} catch (const std::exception& error) {
			std::cerr << "civic new: " << error.what() << '\n';
			return 1;
		}
	}
	if (command == "test") {
		try {
			return runTests(std::vector<std::string>(argv + 2, argv + argc),
							standardLibraryPath);
		} catch (const std::exception& error) {
			std::cerr << "civic test: " << error.what() << '\n';
			return 1;
		}
	}
	if (command == "check") {
		if (argc != 3 || fs::path(argv[2]).extension() != ".cv") {
			std::cerr << "civic check: expected a single .cv source file\n";
			return 2;
		}
		try {
			fs::path sourcePath	   = fs::weakly_canonical(argv[2]);
			// Use parseSourceText (not parseSourceFile) so #header files check
			// as declaration-only modules without a main() requirement.
			civic::Program program = civic::parseSourceText(
				readText(sourcePath), sourcePath.string(),
				standardLibraryPath.string());
			civic::validateProgram(program, !program.isHeader);
			return 0;
		} catch (const civic::Diagnostic& diagnostic) {
			printDiagnostic(diagnostic, fs::current_path() / argv[2]);
			return 1;
		} catch (const std::exception& error) {
			std::cerr << "civic check: " << error.what() << '\n';
			return 1;
		}
	}
	if (command == "fmt") {
		bool checkMode = false;
		bool stdinMode = false;
		std::vector<std::string> files;
		for (int index = 2; index < argc; ++index) {
			const std::string argument = argv[index];
			if (argument == "--check") {
				checkMode = true;
			} else if (argument == "--stdin" || argument == "-") {
				stdinMode = true;
			} else if (argument.rfind("--", 0) == 0) {
				std::cerr << "civic fmt: unknown option '" << argument << "'\n";
				return 2;
			} else {
				files.push_back(argument);
			}
		}
		auto formatted = [](const std::string& path) {
			return civic::formatProgram(
				civic::parseFileText(readText(path), path));
		};
		if (stdinMode) {
			if (!files.empty()) {
				std::cerr << "civic fmt: --stdin takes no file arguments"
							 " (pass a file first to redirect)\n";
				return 2;
			}
			const std::string text{std::istreambuf_iterator<char>(std::cin),
								   std::istreambuf_iterator<char>()};
			try {
				const std::string result =
					civic::formatProgram(civic::parseFileText(text, "<stdin>"));
				std::cout << result;
				if (checkMode && result != text)
					return 1;
				return 0;
			} catch (const civic::Diagnostic& diagnostic) {
				printDiagnosticText(diagnostic, text);
				return 1;
			}
		}
		if (files.empty()) {
			std::cerr << "civic fmt: expected at least one .cv file\n";
			return 2;
		}
		int unformatted = 0;
		for (const std::string& file : files) {
			if (file.empty() || fs::path(file).extension() != ".cv") {
				std::cerr << "civic fmt: '" << file << "' is not a .cv file\n";
				return 2;
			}
			try {
				const std::string text	 = readText(file);
				const std::string result = formatted(file);
				if (checkMode) {
					if (result != text) {
						std::cout << file << '\n';
						++unformatted;
					}
					continue;
				}
				if (result == text)
					continue;
				std::ofstream stream(file, std::ios::binary | std::ios::trunc);
				stream << result;
				if (!stream) {
					std::cerr << "civic fmt: cannot write " << file << '\n';
					return 1;
				}
			} catch (const civic::Diagnostic& diagnostic) {
				printDiagnostic(diagnostic, fs::current_path() / file);
				return 1;
			} catch (const std::exception& error) {
				std::cerr << "civic fmt: " << error.what() << '\n';
				return 1;
			}
		}
		return unformatted == 0 ? 0 : 1;
	}
	if (command != "build" && command != "run" && command != "emit-ir") {
		std::cerr << "civic: unknown command '" << command << "'\n";
		usage(std::cerr);
		return 2;
	}
	fs::path input;
	try {
		if (argc >= 3 && fs::path(argv[2]).extension() == ".cv") {
			input = argv[2];
			fs::path output;
			if (command == "emit-ir") {
				output = input;
				output.replace_extension(".ll");
			} else {
				output = input;
				output.replace_extension("");
				output += executableSuffix();
			}
			std::vector<std::string> programArguments;
			for (int i = 3; i < argc; ++i) {
				if (std::string(argv[i]) == "-o" && command != "run" &&
					i + 1 < argc) {
					output = argv[++i];
				} else if (std::string(argv[i]) == "--" && command == "run") {
					for (++i; i < argc; ++i)
						programArguments.emplace_back(argv[i]);
					break;
				} else {
					std::cerr << "civic: unexpected argument '" << argv[i]
							  << "'\n";
					return 2;
				}
			}
			civic::Program program = civic::parseSourceFile(
				input.string(), standardLibraryPath.string());
			civic::validateProgram(program);
			std::string ir =
				civic::generateLLVMIR(program, input.filename().string());
			if (command == "emit-ir") {
				writeText(output, ir);
				return 0;
			}
			if (command == "run") {
				TemporaryDirectory temporary;
				fs::path executable =
					temporary.path() / ("program" + executableSuffix());
				writeText(temporary.path() / "program.ll", ir);
				std::vector<std::string> compile{
					compiler(), "-x",
					"ir",		(temporary.path() / "program.ll").string(),
					"-o",		executable.string()};
				int result = runProcess(compile);
				if (result != 0)
					return result;
				std::vector<std::string> runArguments{
					fs::absolute(executable).string()};
				runArguments.insert(runArguments.end(),
									programArguments.begin(),
									programArguments.end());
				return runProcess(runArguments);
			}
			TemporaryDirectory temporary;
			fs::path irPath = temporary.path() / "program.ll";
			writeText(irPath, ir);
			int result = runProcess({compiler(), "-x", "ir", irPath.string(),
									 "-g", "-o", output.string()});
			if (result != 0)
				return result;
			std::cout << "built " << output.string() << '\n';
			return 0;
		}

		if (command == "emit-ir") {
			std::cerr << "civic emit-ir: expected a source .cv file\n";
			return 2;
		}
		std::vector<std::string> arguments;
		for (int i = 2; i < argc; ++i)
			arguments.emplace_back(argv[i]);
		return runProjectCommand(command, arguments, input,
								 standardLibraryPath);
	} catch (const civic::Diagnostic& diagnostic) {
		if (input.empty())
			input = fs::current_path() / "src" / "main.cv";
		printDiagnostic(diagnostic, input);
		return 1;
	} catch (const std::exception& error) {
		std::cerr << "civic " << command << ": " << error.what() << '\n';
		return 1;
	}
}
