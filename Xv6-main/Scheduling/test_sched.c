#include "types.h"
#include "stat.h"
#include "user.h"

int main()
{
    int rand_val[15] = {30, 20, 10, 40, 70, 11, 35, 39, 60, 61, 73, 77, 41, 15, 25};
    int burst_val[10];
    int x = getpid() % 15;
    for (int i = 0; i < 10; i++)
    {
        burst_val[i] = rand_val[x];
        x = (x + 1) % 15;
    }
    printf(1, "PID\t Process type\t     Burst Time\tContext Switches\n");
    printf(1, "---\t ------------\t     ----- ----\t------- --------\n");
    for (int i = 0; i < 10; i++)
    {
        int pid = fork();
        if (pid == 0)
        {
            set_burst_time(burst_val[i]);
            if (i % 2 == 0)
            {
                double temp = 0;
                for (int j = 0; j < 500000 * burst_val[i]; j++)
                {
                    temp += 1.12 * 2.71;
                }
                int computed_val = ((int)temp) % 9000 + 1000;
                printf(1, "%d\t CPU Bound(%d) \t", getpid(), computed_val);
            }
            else
            {
                for (int j = 0; j < 10 * burst_val[i]; j++)
                {
                    sleep(1);
                }
                printf(1, "%d\t IO Bound    \t\t", getpid());
            }

            struct processInfo *procinfo = (struct processInfo *)malloc(sizeof(struct processInfo));
            getProcInfo(getpid(), procinfo);
            printf(1, " %d \t  %d\n", get_burst_time(), procinfo->numberContextSwitches);
            exit();
        }
    }

    while (wait() != -1)
        ;

    exit();
}
