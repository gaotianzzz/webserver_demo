// 通过libevent 编写web服务器
#include <stdio.h>
#include <dirent.h>
#include <unistd.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <event.h>
#include <event2/listener.h>

#include "pub.h"

#define _DIR_PREFIX_FILE_ "./dir_header.html"
#define _DIR_TAIL_FILE_ "./dir_tail.html"

typedef struct Base_Info
{
    struct event_base *base;
    struct sockaddr_in *serv;

} Base_Info;

int copy_header(struct bufferevent *bev, int num, char *info, char *filetype, long filesize)
{
    char buf[4096] = {0};
    sprintf(buf, "HTTP/1.1 %d %s\r\n", num, info);
    sprintf(buf, "%sContent-Type: %s\r\n", buf, filetype);
    if (filesize >= 0)
    {
        sprintf(buf, "%sContent-Length:%ld\r\n", buf, filesize);
    }
    strcat(buf, "\r\n");
    bufferevent_write(bev, buf, strlen(buf));
    return 0;
}

int copy_file(struct bufferevent *bev, const char *strFile)
{
    int fd = open(strFile, O_RDONLY);
    char buf[1024] = {0};
    int ret;
    while ((ret = read(fd, buf, sizeof(buf))) > 0)
    {
        bufferevent_write(bev, buf, ret);
    }
    close(fd);
    return 0;
}

// 发送目录，实际上组织一个html页面发给客户端，目录的内容作为列表显示
int send_dir(struct bufferevent *bev, const char *strPath)
{
    // 需要拼出来一个html页面发送给客户端
    copy_file(bev, _DIR_PREFIX_FILE_);
    // send dir info
    DIR *dir = opendir(strPath);
    if (dir == NULL)
    {
        perror("opendir");
        return -1;
    }
    char bufline[1024] = {0};
    struct dirent *dent = NULL;
    struct stat _stat;
    while (dent = readdir(dir))
    {
        stat(dent->d_name, &_stat);
        if (DT_DIR == dent->d_type)
        {
            // 目录文件 特殊处理（<a href='%s/'>%32s</a>）
            memset(bufline, 0x00, sizeof(bufline));
            sprintf(bufline, "<li><a href='%s/'>%32s</a>   %8ld</li>", dent->d_name, dent->d_name, _stat.st_size);
            bufferevent_write(bev, bufline, strlen(bufline));
        }
        else if (DT_REG == dent->d_type)
        {
            // 普通文件 直接显示列表即可（<a href='%s'>%32s</a>）
            memset(bufline, 0x00, sizeof(bufline));
            sprintf(bufline, "<li><a href='%s'>%32s</a>     %8ld</li>", dent->d_name, dent->d_name, _stat.st_size);
            bufferevent_write(bev, bufline, strlen(bufline));
        }
    }
    closedir(dir);
    copy_file(bev, _DIR_TAIL_FILE_);
    // bufferevent_free(bev);
    return 0;
}

int http_request(struct bufferevent *bev, char *path)
{
    // 将中文问题转码成utf-8格式的字符串
    strdecode(path, path);
    char *strPath = path;

    // 默认设置为访问目录
    if (0 == strcmp(strPath, "/") || 0 == strcmp(strPath, "/."))
    {
        strPath = "./";
    }
    else
    {
        strPath = path + 1;
    }

    // 创建文件信息结构体
    struct stat _stat;
    // 该文件不存在 ，提供404页面
    if (stat(strPath, &_stat) < 0)
    {
        copy_header(bev, 404, "NOT FOUND", get_mime_type("error.html"), -1);
        copy_file(bev, "error.html");

        return -1;
    }
    // 处理目录
    if (S_ISDIR(_stat.st_mode))
    {
        copy_header(bev, 200, "OK", get_mime_type("*.html"), _stat.st_size);
        send_dir(bev, strPath);
    }
    // 处理文件
    if (S_ISREG(_stat.st_mode))
    {
        copy_header(bev, 200, "OK", get_mime_type(strPath), _stat.st_size);
        copy_file(bev, strPath);
    }

    return 0;
}

void read_cb(struct bufferevent *bev)
{
    char buf[256] = {0};
    char method[10], path[256], protocol[10];
    int ret = bufferevent_read(bev, buf, sizeof(buf));
    if (0 > ret)
    {
        printf("bufferevent error");
        return;
    }
    else if (0 < ret)
    {
        sscanf(buf, "%[^ ] %[^ ] %[^ \r\n]", method, path, protocol);
        if (strcasecmp(method, "get") == 0)
        {
            // 获取报文第一行: GET、文件名路径、协议
            write(STDOUT_FILENO, buf, ret);

            // 确保数据读完
            char bufline[256];
            while (0 < (ret = bufferevent_read(bev, bufline, sizeof(bufline))))
            {
                write(STDOUT_FILENO, bufline, ret);
            }

            // 处理请求
            http_request(bev, path);
        }
    }
}
void event_cb(struct bufferevent *bev, short what)
{
    // 客户端关闭
    if (what & BEV_EVENT_EOF)
    {
        printf("client closed\n");
        bufferevent_free(bev);
    }
    // 客户端发生错误
    else if (what & BEV_EVENT_ERROR)
    {
        printf("err to client closed\n");
        bufferevent_free(bev);
    }
    // 连接成功
    else if (what & BEV_EVENT_CONNECTED)
    {
        printf("client connect ok\n");
    }
}

// 如果监听到则调用listen_cb回调函数
void listen_cb(struct evconnlistener *listener, evutil_socket_t fd, struct sockaddr *addr, int socklen, void *arg)
{
    struct Base_Info *bi = (struct Base_Info *)arg;
    char ip[INET_ADDRSTRLEN] = "";
    printf("new client IP:%s PORT:%d has connected......\n",
           inet_ntop(AF_INET, &(bi->serv->sin_addr), ip, INET_ADDRSTRLEN),
           ntohs(bi->serv->sin_port));
    // 定义与客户端通信的bufferevent,将其上树
    struct bufferevent *be = bufferevent_socket_new(bi->base, fd, BEV_OPT_CLOSE_ON_FREE);
    bufferevent_setcb(be, read_cb, NULL, event_cb, bi->base);
    bufferevent_enable(be, EV_READ | EV_WRITE);
}

int main(int argc, char *argv[])
{
    // 切换工作目录
    char workdir[256] = {0};
    strcpy(workdir, getenv("PWD"));
    strcat(workdir, "/web-http");
    printf("%s\n", workdir);
    chdir(workdir);

    // 创建根节点和服务器地址信息结构体的对象
    Base_Info bi;
    // 创建根节点
    bi.base = event_base_new();
    // 初始化服务器地址信息结构体
    bi.serv = malloc(sizeof(struct sockaddr_in));
    if (NULL == bi.serv)
    {
        perror("malloc");
        return 1;
    }
    else
    {
        // 内存分配成功
        memset(bi.serv, 0, sizeof(struct sockaddr_in)); // 初始化服务器地址信息
        bi.serv->sin_family = AF_INET;
        bi.serv->sin_port = htons(8888);
        bi.serv->sin_addr.s_addr = htonl(INADDR_ANY);
    }

    // 连接监听器
    struct evconnlistener *listener = evconnlistener_new_bind(bi.base,
                                                              listen_cb, (void *)&bi,
                                                              -1,
                                                              LEV_OPT_CLOSE_ON_FREE | LEV_OPT_REUSEABLE,
                                                              (struct sockaddr *)bi.serv, sizeof(struct sockaddr_in));

    // 循环监听
    event_base_dispatch(bi.base);

    // 释放服务器地址信息结构体
    free(bi.serv);

    // 释放根节点
    event_base_free(bi.base);

    // 释放链接监听器
    evconnlistener_free(listener);

    return 0;
}