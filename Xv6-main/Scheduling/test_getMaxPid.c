#include "types.h"
#include "stat.h"
#include "user.h"

int main(void)
{
    printf(1,"Highest PID of an active process is %d\n",getMaxPid());
	exit();
}