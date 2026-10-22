# Test definition: network
TEST_network_GROUP := Networking
TEST_network_DESC := IPv4 and IPv6 loopback and VirtIO gateway ping
TEST_network_BOOT_ARGS := net.test=1
TEST_network_PASS := net_test: IPv4/IPv6 loopback and VirtIO gateway echo passed
