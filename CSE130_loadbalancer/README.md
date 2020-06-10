# README

## Gail Dylan Salak 1583241
### loadbalancer.c
- Program acts as a loadbalancer between clients and servers. It will determine the server with the least load through intermittent healthchecks and send that server requests.
- run "make" to compile; "make spotless" to clean                           
1) Run one or more instances of httpserver:
- usage: ./httpserver <port> [-L] [-N threads] [-l log-file] [-e num-err] [-    t total-req]
-- -L Run server as if logging was enabled, does not actually log
-- -e Number of errors in 'healthcheck' file, requires -L or -l.
-- -t Number of requests total in 'healthcheck' file, requires -L or -l
2) Run loadbalancer:
- Usage: ./loadbalancer client_port# server_port(s)# 
-- Flag option: -N [#threads] //without -N flag, #threads defaults to 4
-- Flag option: -R [#requests] //without -R flag, program defaults to health    checking every 5 requests
-- Port numbers can be any port number above 1024.
-- Client port is always the first port.
-- Must be at least one server port.
3) Send requests to loadbalancer:
-- ex. curl -s http://localhost/[client_port#]/filename

Limitations: 
-- The program must be run on Linux or else it will not work. 
-- Unsure about the X value as different values lead to differrent results in the tests


