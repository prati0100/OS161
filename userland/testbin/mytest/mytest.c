#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <test161/test161.h>

extern int errno;

int main(int argc, char const *argv[]) {
  (void) argc;
  (void) argv;
  int pid;
	printf("About to fork!\n");
	pid = fork();
	if (pid < 0) {
		printf("fork pid incorrect\n");
    return 0;
	}
  if(pid == 0) {
    /* child */
    int result;
    char *args[3];
    args[0] = (char *)"Hello";
    args[1] = (char *)"World";
    args[2] = NULL;
    for(int i = 0; i < 2; i++)
    {
      printf("%s\n", args[i]);
    }
    printf("About to exec\n");
    result = execv("testbin/testprog", args);
    if(result)
    {
      printf("execv failed. errno: %d\n", errno);
      return 25;
    }
  }
  else {
    int status;
    waitpid(pid, &status, 0);
    printf("Status returned: %d\n", status);
  }
  return 0;
}
