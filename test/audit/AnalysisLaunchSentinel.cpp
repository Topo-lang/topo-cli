#include <filesystem>
#include <fstream>

// A native stand-in makes unintended execution observable on every platform.
// Use the executable's directory so a cleared environment cannot hide a launch.
int main(int argc, char** argv) {
    if (argc > 0)
        std::ofstream(std::filesystem::absolute(argv[0]).parent_path() / "unexpected-execution")
            << "analysis tool was executed\n";
    return 86;
}
