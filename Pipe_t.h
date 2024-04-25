#pragma once
#include <unistd.h>

struct Pipe_t
{
    Pipe_t()
    {
        pipe(fd_s());
    }

    ~Pipe_t()
    {
        Close();
    }

    using fd_t = int;
    using ref_fds_t = fd_t (&)[2];
    using cpipe_t = const fd_t (&)[2];
    fd_t& read_end() {return fd[0];}
    fd_t& write_end() {return fd[1];}

    fd_t read_end() const {return fd[0];}
    fd_t write_end() const {return fd[1];}

    ref_fds_t fd_s() {return fd;}
    cpipe_t fd_s() const {return fd;}

    int Close_write_end() {auto ret = close(write_end()); write_end() = -1; return ret;}
    int Close_read_end() {auto ret = close(read_end()); read_end() = -1; return ret;}
    void Close() 
    {
        Close_write_end();
        Close_read_end();
    } 

private:
    int fd[2];
};