# README

## Gail Dylan Salak 1583241
### httpserver.c
- Program multiplexes the HTTP server to synchronously work on multiple requests. It has the ability to log in a log file and consequently perform healthchecks.
- run "make" to compile; "make spotless" to clean
- Usage: ./httpserver #port 
-- Flag option: -N [#threads] //without -N flag, #threads defaults to 4
-- Flag option: -l [logfile] //without -l flag, there is no logging
-- Port number can be any port number above 1024 from where the client will send their message.
- A healthcheck request can be sent like a GET request to a file named healthcheck
-- healthcheck request format:
    curl -s http://localhost:PORT/healthcheck

Limitations: 
--The program must be run on Linux or else it will not work. 
--There is a bug where the program may hang after multiple PUT requests.
--Logging works on my end, but has not been passing the tests on git for asgn2.

