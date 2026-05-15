#include "pb_validate.h"

int main(int argc, char **argv)
{
    if (argc != 2)
        return 2;
    return pb_validate_file(argv[1]);
}
