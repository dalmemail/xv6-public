#include "types.h"
#include "stat.h"
#include "user.h"
#include "fcntl.h" // for O_WRONLY definition

int
main(int argc, char *argv[])
{
  int fd;

  if(argc != 3){
    exit();
  }

  if ((fd = open(argv[1], O_WRONLY)) < 0) {
	printf(1, "write: cannot open %s\n", argv[1]);
	exit();
  }

  if (write(fd, argv[2], strlen(argv[2])) < 0) {
	printf(1, "write: cannot write %s\n", argv[1]);
	exit();
  }

  close(fd);
  exit();
}
