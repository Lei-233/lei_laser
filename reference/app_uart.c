#include <stdio.h>
#include <termios.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

/* 设置串口参数的函数 */
int set_uart(int fd, int speed, int bits, char check, int stop) {
    struct termios newtio, oldtio;

    // 步骤一：保存原来的串口配置
    if(tcgetattr(fd, &oldtio) != 0) {
        printf("tcgetattr oldtio error\n");
        return -1;
    }

    bzero(&newtio, sizeof(newtio));
    
    // 步骤二：设置控制模式标志
    newtio.c_cflag |= CLOCAL | CREAD;
    newtio.c_cflag &= ~CSIZE;

    // 步骤三：设置数据位
    switch(bits) {
        case 7:
            newtio.c_cflag |= CS7;
            break;
        case 8:
            newtio.c_cflag |= CS8;
            break;
    }
    
    // 步骤四：设置奇偶校验位
    switch(check) {
        case 'O': // 奇校验位
            newtio.c_cflag |= PARENB;   // 使能校验
            newtio.c_cflag |= PARODD;   // 奇校验
            newtio.c_iflag |= (INPCK | ISTRIP); // 输入校验使能
            break;
        case 'E': // 偶校验位
            newtio.c_cflag |= PARENB;   // 使能校验
            newtio.c_cflag &= ~PARODD;  // 偶校验
            newtio.c_iflag |= (INPCK | ISTRIP); // 输入校验使能
            break;
        case 'N': // 无校验
            newtio.c_cflag &= ~PARENB;
            break;
    }
    
    // 步骤五：设置波特率
    switch(speed) {
        case 9600:
            cfsetispeed(&newtio, B9600);
            cfsetospeed(&newtio, B9600);
            break;
        case 115200:
            cfsetispeed(&newtio, B115200);
            cfsetospeed(&newtio, B115200);
            break;
    }
    
    // 步骤六：设置停止位
    switch(stop) {
        case 1:
            newtio.c_cflag &= ~CSTOPB; // 1位停止位
            break;
        case 2:
            newtio.c_cflag |= CSTOPB; // 2位停止位
            break;
    }
    
    // 步骤七：刷新输入队列
    tcflush(fd, TCIFLUSH);
    
    // 步骤八：设置配置立刻生效
    if (tcsetattr(fd, TCSANOW, &newtio) != 0) {
        printf("tcsetattr newtio error\n");
        return -2;
    }
    
    return 0;
}

int main(int argc, char *argv[])
{
    int fd;
    char buf[255];
    int count;
    fd = open("/dev/ttyS3", O_RDWR | O_NOCTTY | O_NDELAY);
    if(fd == -1) {
        printf("Cannot open /dev/ttyS3\n");
        return -1;
    }
    set_uart(fd, 115200, 8, 'N', 1);

    write(fd, argv[1], strlen(argv[1]));
    sleep(1);

    count = read(fd, buf, sizeof(buf));
    buf[count] = '\0';
    printf("Received: %.*s\n", count, buf);

    close(fd);

    return 0;
}