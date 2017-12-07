#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <err.h>
#include <test161/test161.h>

int main(int argc, char const *argv[]) {
  printf("Inside testprog\n");
  printf("argc = %d\n", argc);
  for(int i = 0; i < argc; i++)
  {
    printf("%s\n", argv[i]);
  }
  exit(50);
}
