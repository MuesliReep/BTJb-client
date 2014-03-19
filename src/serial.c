#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <termios.h>

#define LGPFX "SERIAL:"

//#define DEVICE "/dev/pts/3"	//TODO: this should be in config file

struct termios tio;
struct termios stdio;
struct termios old_stdio;
int tty_fd;

int serial_init(const char *file) {

	tcgetattr(STDOUT_FILENO,&old_stdio);

	printf("Opening device %s\n",file);
	Log(LGPFX" Opening serial device %s.\n",file);
	memset(&stdio,0,sizeof(stdio));
	stdio.c_iflag=0;
	stdio.c_oflag=0;
	stdio.c_cflag=0;
	stdio.c_lflag=0;
	stdio.c_cc[VMIN]=1;
	stdio.c_cc[VTIME]=0;
	tcsetattr(STDOUT_FILENO,TCSANOW,&stdio);
	tcsetattr(STDOUT_FILENO,TCSAFLUSH,&stdio);
	fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK);       // make the reads non-blocking

	memset(&tio,0,sizeof(tio));
	tio.c_iflag=0;
	tio.c_oflag=0;
	tio.c_cflag=CS8|CREAD|CLOCAL;           // 8n1, see termios.h for more information
	tio.c_lflag=0;
	tio.c_cc[VMIN]=1;
	tio.c_cc[VTIME]=5;

	tty_fd=open(file, O_RDWR | O_NONBLOCK);
	cfsetospeed(&tio,B115200);            // 115200 baud
	cfsetispeed(&tio,B115200);            // 115200 baud

	tcsetattr(tty_fd,TCSANOW,&tio);


	return 0; //TODO: return real result
}

int serial_read(unsigned char *c){

	unsigned char d;

	if (read(tty_fd,&d,1)>0){
		*c = d;
		return 1;
	}

	return 0;
}

int serial_write(unsigned char *c){

	unsigned char d= *c;

	write(tty_fd,&d,1);

	return 0;
}

void serial_exit(){
	close(tty_fd);
	tcsetattr(STDOUT_FILENO,TCSANOW,&old_stdio);
}
