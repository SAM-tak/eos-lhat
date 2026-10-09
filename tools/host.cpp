// Minimal L^ host for signature generation and extension lifecycle tests.
#include <lhat.h>
#include <lhat/extension.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace {
std::string loaderError;
void *openLibrary(void *, const char *path) {
#ifdef _WIN32
    auto absolute = std::filesystem::absolute(std::filesystem::u8path(path));
    auto module = LoadLibraryExW(absolute.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!module)
        loaderError = "Windows loader error " + std::to_string(GetLastError());
#else
    auto module = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!module)
        loaderError = dlerror();
#endif
    return module;
}
LhatExtensionSymbol symbol(void *, void *module, const char *name) {
#ifdef _WIN32
    return reinterpret_cast<LhatExtensionSymbol>(GetProcAddress(static_cast<HMODULE>(module), name));
#else
    return reinterpret_cast<LhatExtensionSymbol>(dlsym(module, name));
#endif
}
void closeLibrary(void *, void *module) {
#ifdef _WIN32
    FreeLibrary(static_cast<HMODULE>(module));
#else
    dlclose(module);
#endif
}
const char *error(void *) { return loaderError.c_str(); }
void freeExtensions(LhatExtensions *pool) {
    // Every machine and Program has gone before registry callbacks and DLLs.
    lhat_registry_dispose();
    lhat_extensions_free(pool);
}
void require(bool ok, const std::string &message) {
    if (!ok)
        throw std::runtime_error(message);
}
void writeBytes(const char *path, uint8_t *bytes, size_t size) {
    std::unique_ptr<uint8_t, decltype(&lhat_free)> owned(bytes, lhat_free);
    std::ofstream output(std::filesystem::u8path(path), std::ios::binary);
    output.write(reinterpret_cast<const char *>(bytes), static_cast<std::streamsize>(size));
    output.close();
    require(bool(output), std::string("Could not write ") + path);
}
void diagnose(LhatProgram *program) {
    for (size_t i = 0; i < lhat_program_diagnostic_count(program); ++i) {
        auto diagnostic = lhat_program_diagnostic(program, i);
        std::cerr << diagnostic->path << ": program error " << diagnostic->code << '\n';
    }
    for (auto unit = lhat_program_units(program); unit; unit = lhat_unit_next(unit)) {
        for (size_t j = 0; j < lhat_unit_diagnostic_count(unit); ++j) {
            char message[2048];
            lhat_unit_diagnostic_write(unit, j, false, message, sizeof(message));
            std::cerr << message << '\n';
        }
    }
}
void checkRun(const LhatRunResult &result) {
    if (result.status != LHAT_RUN_OK) {
        char value[2048];
        lhat_value_write(result.value, value, sizeof(value));
        throw std::runtime_error("L^ execution failed: " + std::string(value));
    }
}
} // namespace

int main(int argc, char **argv) {
    // Explicit paths only. This executable is build/test tooling, not a game host.
    if (argc < 4) {
        std::cerr << "Usage: eos_lhat_host signatures LIBRARY OUTPUT\n"
                     "       eos_lhat_host compile LIBRARY SOURCE OUTPUT\n"
                     "       eos_lhat_host run LIBRARY UNIT [REPETITIONS]\n";
        return 1;
    }
    try {
        const std::string mode = argv[1];
        require((mode == "signatures" && argc == 4) ||
                    (mode == "compile" && argc == 5) ||
                    (mode == "run" && (argc == 4 || argc == 5)), "Invalid arguments");
        const int repetitions = mode == "run" && argc == 5 ? std::stoi(argv[4]) : 1;
        require(repetitions > 0 && repetitions <= 100, "Invalid repetition count");
        LhatExtensionLoader loader{nullptr, openLibrary, symbol, closeLibrary, error};
        std::unique_ptr<LhatExtensions, decltype(&freeExtensions)> pool(
            lhat_extensions_new(&loader), freeExtensions);
        require(bool(pool), "Could not allocate extension pool");
        const auto module = lhat_extensions_load(pool.get(), argv[2]);
        require(module != nullptr, lhat_extensions_error(pool.get()));
        for (int iteration = 0; iteration < repetitions; ++iteration) {
            // A restart discards the entire VM and Program, retaining the loaded DLL.
            std::unique_ptr<LhatProgram, decltype(&lhat_program_free)> program(
                lhat_program_new(true, lhat_load_file, nullptr), lhat_program_free);
            require(bool(program), "Could not allocate Program");
            if (!lhat_extensions_register(pool.get(), program.get(), &module, 1))
                throw std::runtime_error(lhat_extensions_error(pool.get()));
            uint8_t *bytes = nullptr;
            size_t size = 0;
            if (mode == "signatures") {
                require(lhat_program_write_signatures(program.get(), &bytes, &size),
                        "Could not write signatures (a full host is required)");
                writeBytes(argv[3], bytes, size);
                continue;
            }
            const auto unit = lhat_program_check(program.get(), argv[3]);
            if (!unit || !lhat_program_compile(program.get())) {
                diagnose(program.get());
                throw std::runtime_error("Could not compile/load unit");
            }
            if (mode == "compile") {
                require(lhat_unit_write_binary(unit, true, &bytes, &size) == LHAT_WRITE_OK,
                        "Could not serialize unit");
                writeBytes(argv[4], bytes, size);
                continue;
            }
            std::unique_ptr<LhatMachine, decltype(&lhat_machine_dispose)> machine(
                lhat_machine_new(), lhat_machine_dispose);
            require(bool(machine) && lhat_program_install(program.get(), machine.get()),
                    "Could not install Program");
            auto result = lhat_run(machine.get(), lhat_unit_proto(unit));
            checkRun(result);
            result = lhat_machine_call_member(machine.get(), result.value, "run", 3, nullptr, 0);
            checkRun(result);
            require(lhat_is_number(result.value) && lhat_number_as_real(result.value) == 0,
                    "Test must return zero");
        }
        std::cout << "PASS: " << mode << " (" << repetitions << " Program(s))\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
