# Commands run by the launchd job com.hackios.demo: the modem's session as iOS's network (en1): configured by configd over DHCP (the kernel
# answers with the session's address), a name resolved, a page over plain TCP and over CFNetwork's HTTPS, with no Wi-Fi.
/hackios/bin/net_test wait 65 show dhcp en1 waitaddr en1 40
/hackios/bin/net_test resolve example.com tcp example.com 80 reach example.com
/hackios/bin/net_test fetch https://www.apple.com/library/test/success.html
/hackios/bin/net_test show wait 20 fetch https://example.com/
