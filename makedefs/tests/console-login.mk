# Test definition: console-login
TEST_console-login_GROUP := Kernel
TEST_console-login_DESC := A normal boot logged into on the serial console: getty and login on /dev/console, a wrong password, user and root sessions, the terminal handed over and back
TEST_console-login_PASS := serial_expect.pl: script complete|bootd: com.nxu.getty: started|Login incorrect|uid 1000  gid 4|uid 0  gid 4
TEST_console-login_TIMEOUT := 120
TEST_console-login_SERIAL_SCRIPT := tools/console_login.script
