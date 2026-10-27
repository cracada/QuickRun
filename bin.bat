windres app.rc -o app_res.o --codepage=65001
gcc QuickRun.c app_res.o -o QuickRun.exe -Os -mwindows -s -lshell32 -ldwmapi -lmsimg32 ^
    -Wall -Wextra -Wshadow -Wformat=2 