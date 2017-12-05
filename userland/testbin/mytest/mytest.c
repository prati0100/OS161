#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <test161/test161.h>

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
    char str[] = "Inside child.";
    int fd;
    fd = open("child.txt", (O_RDWR | O_CREAT));
    write(fd, str, strlen(str));
    return 0;
  }
  else {
    char str[] = "Inside parent. Child's pid is: ";
    int fd;
    fd = open("parent.txt", (O_RDWR | O_CREAT));
    write(fd, str, strlen(str));
    return 0;
  }

  return 0;
}
