#include "types.h"
#include "stat.h"
#include "user.h"
#include "x86.h"

int generate_vals(int x){
	return (x*x) + x + 1; // x^2 - x + 1 
}
int main(int argc, char* argv[]){
	int childno, iterno;

	for(childno = 1; childno <= 10; childno++){
		int pid = fork();

		if(pid < 0){
			printf(1, "Fork failed\n");
			exit();
		}

		if( pid == 0 ){
			// printf(1, "Child %d\n", childno);
			for(iterno = 1; iterno <= 10; iterno++){
				int *arr = malloc(4096);
				// filling values
				for(int k=0;k<1024;k++){
					arr[k] = generate_vals(k);
				}

				int validated=0;
				// validating with the same function
				for(int k=0;k<1024;k++){
					if(arr[k] == generate_vals(k))
						validated++;
				}
				if(validated == 1024){
					printf(1, "Child %d -> Iteration : %d\t Validation successful\n", childno,iterno);
				} else {
					printf(1, "Iteration : %d\t Validation failed, not matched %dB\n", 
						iterno, (4096 - (validated*4))
					);
				}
			}
			printf(1, "\n");
			exit();
		}
	}

	while(wait()!=-1);
	exit();

}