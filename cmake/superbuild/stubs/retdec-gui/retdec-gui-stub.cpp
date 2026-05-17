#include <cstring>
#include <iostream>

int main(int argc, char **argv)
{
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--self-test") == 0) {
            return 0;
        }
    }

    std::cout << "retdec-gui stub: Qt/OpenCL/ML GUI will be implemented later.\n";
    return 0;
}

