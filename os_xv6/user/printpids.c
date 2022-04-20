#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char **argv)
{
    print_pids();
    print_stats();
    printf("%d\n", get_utilization());
    exit(0);
}